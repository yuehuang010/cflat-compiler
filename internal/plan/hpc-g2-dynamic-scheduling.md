# G2: Dynamic Scheduling for parallel_for_n

Status: DONE for steps 1-5 (2026-07-09, integrated on master, gates green).
Step 6 / Phase 2 (parallel_reduce_dynamic) NOT started - see notes at end.
Created: 2026-07-09
Parent: internal/plan/hpc-gaps.md (G2)
Files: cflat/core/hpc/parallel.cb (only), doc/HPC.md, Test/test_hpc_kernels.cb

## Problem

Both existing helpers (parallel_for_n / parallel_reduce, raw and _pool) use
STATIC scheduling: [0, n) is split once, up front, into exactly `workers`
contiguous near-equal ranges, one lambda invocation per worker. (The pool
variants submit one big range-task per worker, so the division is still
fixed before any work runs - the pool's task queue being dynamic does not
help.)

Static is optimal when cost-per-index is uniform (axpy, stencil): zero
coordination, perfect balance by construction. It falls over when cost
varies with the index:

- triangular loops (iteration i costs ~n-i): with 4 workers the first
  quarter holds ~44% of the work; wall time = the unluckiest worker,
  ~2.3x speedup instead of ~4x
- CSR rows with skewed nnz distributions
- shrinking trailing updates (LU/Cholesky) - doc/HPC.md currently fights
  this with a serial-fallback work cutoff precisely because static
  splitting handles it poorly

## Design

Add DYNAMIC scheduling variants: cut [0, n) into many chunks (far more
chunks than workers) and have workers pull the next chunk from a shared
atomic counter when they finish their current one. A worker that drew
cheap chunks just draws more; balance is automatic for any cost skew, and
nobody idles until the last chunk is handed out.

### New entry points (core/hpc/parallel.cb)

```
void parallel_for_n_dynamic(i64 n, int workers, i64 chunk,
                            Lambda<void(i64, i64)> body,
                            bool pin = false, int fpConfig = 0);

void parallel_for_n_dynamic_pool(ThreadPool* pool, i64 n, int workers,
                                 i64 chunk, Lambda<void(i64, i64)> body);
```

- Same body shape as parallel_for_n - the lambda receives [lo, hi) - so a
  kernel migrates by changing one call name. hi - lo == chunk except the
  final chunk, which clamps to n.
- `chunk <= 0` selects an automatic size (below). `workers <= 0` defaults
  as in the static helpers. Distinct names, not overloads (same reason as
  `_pool`: generic overloads of one name collide in mangling; keep the
  whole family uniform).
- Raw variant calls __pfor_raw_hotloop_check() like its static sibling.

Phase 2 (after for_n lands and measures well):

```
T parallel_reduce_dynamic<T>(i64 n, int workers, i64 chunk, T identity,
                             Lambda<T(i64, i64)> partial,
                             Lambda<T(T, T)> combine,
                             bool pin = false, int fpConfig = 0);
T parallel_reduce_dynamic_pool<T>(...);
```

Each worker folds every chunk it pulls into ONE local accumulator
(seeded with identity, merged with combine), writes it to its distinct
partials slot; the helper merges `workers` partials at the end, exactly
like the static reduce. One combine per chunk, one slot write per worker.

### Chunk dispenser

Shared `atomic_counter` (core/atomic.cb) owned by the helper's frame.
`add(chunk)` is a relaxed atomicrmw add returning the NEW value, so:

```
loop {
    i64 newv = box->next->add(box->chunk);   // atomic fetch-add
    i64 lo   = newv - box->chunk;
    if (lo >= box->n) { break; }
    i64 hi = lo + box->chunk;
    if (hi > box->n) { hi = box->n; }
    box->body(lo, hi);
}
```

Relaxed ordering is sufficient: the range bounds are data-dependent on
the returned value, the counter guards no other data, and the workers'
writes are published to the caller by the existing join/wait barrier.
Overshoot is benign - late pullers see lo >= n and exit; i64 cannot
plausibly wrap.

### Worker plumbing (mirrors the static helpers)

New box struct alongside __PForBox:

```
struct __PForDynBox {
    Lambda<void(i64, i64)> body = default;
    atomic_counter* next = nullptr;   // shared dispenser (caller frame)
    i64 n     = 0;
    i64 chunk = 0;
};
```

- RAW: one __PForDynBox per worker in a helper-owned array (body cloned
  per box, as today - the clone is what each thread's closure env rides);
  one thread per worker runs the puller loop; join; `delete[workers]`
  the boxes to destruct the body clones. pin/fpConfig behave exactly as
  in parallel_for_n.
- POOL: one heap box per worker submitted with a puller-loop trampoline
  (same ownership protocol as __pfor_pool_tramp / __pfor_box_free: task
  frees on run, deleter frees on refusal). If submit() is refused, run
  the puller loop INLINE on the calling thread - it just drains whatever
  chunks remain, so the fallback composes correctly with workers that
  did get submitted (unlike the static fallback, which runs one fixed
  range). The atomic_counter lives in the caller's frame and the helper
  waits on all handles before returning, so the pointer never dangles.

### Default chunk size

`chunk <= 0` -> `max(1, n / (workers * 16))`, then clamp to n.

Rationale: ~16 chunks per worker bounds imbalance at roughly 1/16 of one
worker's share (the tail can only be as ragged as one chunk) while
keeping the atomic + dispatch overhead amortized over a meaningful range.
The multiplier is a named constant (PFOR_DYN_CHUNKS_PER_WORKER = 16) so
measurement can move it. Explicit `chunk` is the tuning knob for bodies
with extreme per-index cost (chunk small) or very cheap bodies (chunk
large - amortize the counter line ping-pong; see tradeoff below).

### What is deliberately NOT in scope

- Guided scheduling (shrinking chunks): only if the skewed-work benchmark
  shows dynamic's tail raggedness actually costing something at realistic
  n; record numbers either way.
- Work stealing (per-worker deques): the single shared counter is the
  90% answer at this worker count; stealing is a different plan if a
  workload ever shows the counter as the bottleneck.
- Changing ThreadPool: zero pool/compiler changes; pure library.

## Semantics and caveats (document in doc/HPC.md)

- parallel_for_n_dynamic: every index is processed exactly once; a
  race-free body produces results IDENTICAL to serial (chunking changes
  only which thread computes an index, not any FP order within it).
- parallel_reduce_dynamic: which chunks land in which worker's
  accumulator varies RUN TO RUN, so the FP result is not reproducible
  even at fixed `workers` - strictly weaker than static reduce (which is
  deterministic for fixed workers). Say this plainly; steer users whose
  reduction must be stable to the static variant.
- Tradeoff table for the doc: static = zero coordination, deterministic
  partition, contiguous per-worker ranges (cache/first-touch friendly),
  eats full imbalance; dynamic = one relaxed atomic per chunk on a
  contended counter line, non-deterministic placement, immune to skew.
  Rule of thumb: uniform body -> static; skewed body -> dynamic.
- Body must still be race-free across DIFFERENT indices, same as static.

## Implementation steps

1. __PForDynBox + puller loop + parallel_for_n_dynamic (raw). Static
   helpers untouched.
2. parallel_for_n_dynamic_pool with the refused-submit inline-drain path.
3. Tests (extend Test/test_hpc_kernels.cb - no new test files):
   - correctness: fill y[i] = f(i) via dynamic (raw + pool), compare
     element-exact vs serial; n not divisible by chunk; chunk > n;
     n == 0; workers > n; chunk <= 0 (auto).
   - skew: triangular-cost body, assert result equality vs serial.
4. doc/HPC.md: new subsection under the parallel helpers with the
   static-vs-dynamic tradeoff + chunk guidance; update the LU work-cutoff
   note to mention the dynamic alternative.
5. Benchmark (performance/hpc): triangular loop, static vs dynamic vs
   serial at a few n; record whether guided is warranted.
6. Phase 2: parallel_reduce_dynamic(_pool)<T> re-using the __PRedBox
   dispatch pattern (non-generic header + __pred_run<T>-style runner),
   with the per-worker local-accumulator fold.

## Verification gates

- Rebuild before testing through the exe: parallel.cb is a DEPLOYED core
  library - the compiler reads the copy next to cflat.exe, not the source
  tree. Build Release first, then test.
- test.bat (Release) green; example.bat green (touches nothing but core,
  but cheap to confirm).
- Portability: pure-library change over thread.cb/threadpool.cb/atomic.cb,
  all already ported - re-verify test.sh on WSL green per the repo rule
  for core/*.cb changes.
- Skewed-work benchmark shows dynamic > static on the triangular body and
  static >= dynamic (within noise) on a uniform body at auto chunk.

## Implementation notes (as landed, 2026-07-09)

Deviations from the text above, all deliberate:

1. POOL-REFUSAL FIX: the wording "drain the puller loop before deleting
   the box" above is a use-after-free - ThreadPool.submit frees ctx via
   the deleter BEFORE returning nullptr on refusal. As landed, the inline
   drain runs off still-live locals (body/counter/n/chunk in the helper's
   frame), never the freed box. Any future variant (Phase 2 included)
   must repeat this shape.
2. Tests went into Test/test_parallel.cb, not test_hpc_kernels.cb - that
   is the file that actually imports and exercises hpc/parallel.cb.
3. New benchmark file performance/hpc/pfor_dynamic_bench.cb (none existed
   for the parallel helpers).

Measured (4 workers, triangular skew): n=65536 serial 1463ms, static
639ms (2.29x), dynamic 363ms (4.03x). Uniform body: par at n=2^20,
dynamic ~23% slower at n=2^22 (counter-line contention). Guided variant
NOT warranted on this evidence; revisit only with a new workload.

WSL test.sh re-verification skipped at integration (user decision).

## Phase 2 readiness (parallel_reduce_dynamic<T>, when picked up)

__PRedBox<T> adapts cleanly: add next/n/chunk/combine fields; replace the
single partial() call with a chunk-pulling fold into one local
accumulator, written to the worker's slot after exhaustion. Watch for:
(a) the same pool-refusal use-after-free trap as note 1; (b) the raw
variant's one-shot inline trampoline lambda becomes a small loop - a
second copy of the drain logic to keep in sync.
