# HPC Gaps Plan

Status: G1-G3 AND G5 DONE, G2 incl Phase 2 reduce_dynamic (2026-07-09,
integrated on master, all Windows gates green); G7's rdtsc-timing and
BLAS-example sub-items also DONE (2026-07-09). G4 DONE 2026-07-09
(vectorize(contract)/vectorize(reassoc) tiers - see detailed plan). G6
and the rest of G7 remain PROPOSED.
WSL test.sh re-verification for the core library changes deliberately
skipped (user decision at integration).
Created: 2026-07-09
Source: gap analysis of doc/HPC.md (+ CLI.md, THREADING.md) against what HPC
developers need from a compiler/library. What CFlat already has is strong:
vectorize checked contract, T[]/span/view noalias lattice, simd<T,N> with
masks/select, parallel_for_n/parallel_reduce (+pool, pinning, FP env, RNG
jump/split), --cpu native/--tune, core/hpc kernel library. This plan covers
only what is missing, ranked by value.

Sequencing: G1 and G2 are cheap and unblock user code; G3 and G4 are what
stand between core/hpc and its next 2x; G5-G7 follow.

---

## G1. Alignment control (biggest gap)

Status update 2026-07-09: codebase survey found `alignas(N)` ALREADY
exists end-to-end for structs/classes/fields/locals/globals (grammar
`alignmentSpecifier`, `StructData.UserRequestedAlignment`, tested in
Test/test_basic.cb:3177-3205). Remaining G1 work is only the aligned
`new` clause + assume-aligned; syntax reuses `alignas`, no new `align`
keyword.

G1 DONE 2026-07-09 (agent): `new T[n] alignas(N)` implemented (grammar
array-alt `alignmentSpecifier?`; ParseNewExpression computes allocAlign =
max(site, type) and keeps sizeof/stride on the type effAlign; routes to the
2-arg aligned operator new). Assume-aligned emitted at the alloc site via
`CreateAlignmentAssumption` - .ll shows the align operand bundle and -O2
vectorize loops use `align N` moves (vs align 8 baseline). Aligned free is
sound: per-site alignment is carried on the owning local
(NamedVariable.AllocAlignment, threaded via lastAllocAlignment) so both the
scope-exit auto-free (EmitOwningPtrCleanup) and explicit delete route to
__delete_aligned; escaping the buffer into a field loses the tag (delete it
explicitly there). doc/HPC.md gained an Alignment section. Tests:
Test/test_basic.cb testAlignas + err_new_alignas_not_power_of_two.cb. Gates
green: test.bat, test_lsp.bat (201/0), example.bat (88/0/24).

Today: only alignof. No way to over-align a type/field/allocation, no
assume-aligned. simd load/store uses element alignment (safe, but forfeits
aligned-move codegen).

Work items:
- `align(N)` attribute on struct declarations and fields (N a power-of-two
  literal). Emits LLVM alignment on the type/GEP. Primary use: cache-line
  padding (`align(64)`) to kill false sharing, e.g. per-worker accumulator
  slots - today users would have to hand-count pad bytes.
- Aligned array allocation: `new double[n] align(64)` (or an allocator
  entry point) so kernel buffers can be vector-aligned.
- An assume-aligned the vectorizer can consume, so an aligned buffer's
  loop uses aligned moves. Could ride the T[] view: an aligned `new`
  produces a view whose alignment is carried in metadata.
- Optional follow-on: simd<T,N>.load/store aligned variants
  (`load_aligned`) that assert N*sizeof(T) alignment.

Verification: IR shows `align 64` on allocas/GEPs/loads; a padded
two-thread counter benchmark shows the false-sharing cliff disappear;
extend Test/test_hpc_kernels.cb (or nearest existing test) - no new test
files.

## G2. Dynamic scheduling for parallel_for_n

Detailed plan: internal/plan/hpc-g2-dynamic-scheduling.md (2026-07-09).

G2 DONE 2026-07-09 (agent, ALL steps incl Phase 2:
parallel_reduce_dynamic<T> + _pool landed same day - see detailed plan). parallel_for_n_dynamic +
_pool in core/hpc/parallel.cb; tests in Test/test_parallel.cb (not
test_hpc_kernels.cb - that file does not exercise parallel.cb); benchmark
performance/hpc/pfor_dynamic_bench.cb. Triangular skew, 4 workers,
n=65536: serial 1463ms, static 639ms (2.29x), dynamic 363ms (4.03x -
near-ideal). Uniform body: par at moderate n, ~23% slower at n=2^22
(counter contention - documented tradeoff). Guided variant not warranted
on this evidence.

Today: static balanced chunks only. Irregular work (sparse rows with
varying nnz, adaptive mesh, shrinking LU trailing update - the doc already
fights this with a work cutoff) wants dynamic/guided.

Work items:
- `parallel_for_n_dynamic(_pool)` in core/hpc/parallel.cb: workers pull
  fixed-size chunks from a shared atomic counter until exhausted.
  Chunk size parameter with a sane default; optional guided variant
  (halving chunk sizes) if measurements justify it.
- Same lambda shape as parallel_for_n (body gets [lo, hi)), so kernels
  migrate by changing one call.
- Document the static-vs-dynamic tradeoff (dynamic pays one atomic per
  chunk; static pays imbalance) in doc/HPC.md.

Verification: a skewed-work microbenchmark (e.g. triangular loop) shows
dynamic beating static; results identical to serial for a deterministic
body. Pure library change - no compiler work.

## G3. SIMD completeness

Detailed plan: internal/plan/hpc-g3-simd-completeness.md (2026-07-09).

G3 DONE 2026-07-09 (agent, all 3 phases): reduce_add/min/max (reassoc
fadd), lanes() iota helper (needed - no other way to build a tail mask),
load_masked/store_masked, load_gather/store_scatter (unmasked MVP), all
as ParseSimdStaticMethod branches - no grammar/ParseDeclarationSpecifiers
changes. Tests: test_hpc.cb 141->170 asserts + 4 err_simd_*.cb. -O2 spot
check matches -O0. Shuffles/lane writes stay deferred per plan.

Today: no lane writes, no shuffles (doc says so), no horizontal reduce,
no masked load/store, no gather/scatter.

Work items, in value order:
- `simd<T,N>.reduce_add(v)` (and reduce_min/reduce_max): lowers to
  llvm.vector.reduce.fadd/fmin/fmax (integer: add/smin/umax per
  signedness). Removes the N scalar `v[i]` reads at every dot-product
  tail. Note fadd reduce is ordered vs reassoc - pick reassoc to match
  the vectorize contract's documented semantics, and document it.
- Masked load/store: `simd<T,N>.load_masked(arr, i, mask, passthru)` /
  `store_masked` lowering to llvm.masked.load/store. Kills the scalar
  tail epilogue for n % N.
- Gather/scatter: `load_gather(arr, simd<i32,N> idx)` /
  `store_scatter` via llvm.masked.gather/scatter. Enables CSR spmv and
  particle codes in explicit SIMD.
- Shuffles/lane writes: bigger design question (what syntax, which
  permutes) - defer to its own plan when a kernel actually needs it.

Verification: each op is 1:1 with an LLVM intrinsic - check the .ll;
extend the simd sections of existing tests; re-run performance/hpc
kernels that currently carry scalar tails.

## G4. Scoped FP-math control (FMA contraction + reassociation)

Detailed plan: internal/plan/hpc-g4-vectorize-contract.md (2026-07-09).
RE-SCOPED by user decision: no new `fastmath` keyword - extend the
existing vectorize soft keyword to `vectorize(contract)` and
`vectorize(reassoc)` (two tiers, reassoc implies contract; lock-clause
grammar precedent; builder-default FMF save/set/restore around the loop
body brackets). Serial-FMA escape hatch already exists (scalar fma() in
core/math.cb + simd fma). fast/nsz/arcp/afn/nnan/ninf levels and a
--fp-contract CLI flag stay deferred.

Today: `vectorize` on a reduction is opt-in reassociation for that loop
(tasteful partial answer). But there is no spelling anywhere for FMA
*contraction* (a*b+c -> one vfmadd when the target has FMA; distinct
from simd.fma's single-rounding semantics) or for reassociation outside
a vectorize loop. With --cpu native, contraction alone is a large
fraction of peak FLOPs.

Work items:
- A `fastmath { ... }` block modifier (fits the existing block-modifier
  aesthetic, cf. if const / expect_error) or per-function attribute.
  Sets contract+reassoc fast-math flags on FP instructions emitted in
  the region. Start with contract-only (`fastmath(contract)`) since it
  never changes results beyond removing a rounding; full reassoc as an
  explicit opt-in level.
- Grammar: new rule in CFlat.g4; ForwardRefScanner no-op override if it
  can appear at file scope; both ParseDeclarationSpecifiers copies
  untouched (block-level only).
- Document the interaction with vectorize (a fastmath(contract)
  vectorize loop should emit vfmadd on FMA targets) and with the
  simd.fma caveat in doc/HPC.md.

Verification: .ll shows `contract`/`reassoc` flags scoped to the region;
N-body kernel with --cpu native shows the fmul+fadd -> vfmadd win;
bit-identical results outside the region.

## G5. Topology-aware CPU masks

Detailed plan: internal/plan/hpc-g5-topology-masks.md (2026-07-09).
Pure library change (new core/topology.cb + os.windows.cb externs);
Windows-only by design - POSIX setAffinity is a no-op today, so the
POSIX branch just compiles with a degraded uniform topology.

G5 DONE 2026-07-09 (agent): CpuTopology snapshot (RelationAll Ex-walk,
SDK-verified offsets, L3 dedupe, lazy-mutex memoized) + cpu_mask_physical/
perf_cores/efficiency_cores/llc/numa helpers; testCpuTopology() in
test_threadpool.cb; docs updated. Dev box reports 10 phys/2 LLC/1 NUMA,
EfficiencyClass DOES split Zen5(1) vs Zen5c(0) on this AMD part. Bench
performance/hpc/topology_pingpong_bench.cb: cross-CCX line bounce ~2x
intra-CCX (~190ns vs ~90/105ns roundtrip) - cpu_mask_llc validated.

Today: cpu_mask_lowest(n) assumes cores 0..n-1 are the right set. On
hybrid parts that is wrong by default: on the dev box (Strix Point,
4x Zen5 + 6x Zen5c, split CCX), cpu_mask_lowest(8) spans both core types
and both CCXs - exactly the jitter pinning exists to remove.

Work items:
- Topology query in core (thread.cb or a new core/topology.cb):
  physical core count, SMT sibling map, LLC/CCX domain map, NUMA node
  map. Windows: GetLogicalProcessorInformationEx (RelationCache /
  RelationNumaNode / RelationProcessorCore). Linux: sysfs. Gate with
  if const like os.posix.cb.
- Mask helpers on top: cpu_mask_llc(domain), cpu_mask_physical(n)
  (one thread per physical core, skip SMT siblings),
  cpu_mask_perf_cores() where the OS exposes a core-class distinction.
- Feed the existing machinery: ThreadPool.init pinMask and
  parallel_for_n pin stay unchanged - only the mask construction gets
  smarter.
- Natural landing spot later for per-node arenas if NUMA hardware ever
  matters (single-node dev box: CCX effects are testable, node placement
  is not).

Verification: on the dev box, an intra-CCX pinned producer/consumer vs
cross-CCX shows the known ~3x line-transfer gap; mask helpers return
disjoint, correctly-sized masks on both OSes.

## G6. SoA generic (design project, not gap-fill)

Today: nothing; users hand-write parallel arrays.

Direction: a `soa<T, N>` (or soa<T> + runtime length) generic that
monomorphizes struct {float x,y,z;} into three contiguous arrays with
p[i].x access sugar. Rides the existing monomorphization machinery
(Box<int> -> Box__int); fields are known at instantiation, so no
reflection needed (consistent with the no-type-meta-system decision).
Each field array is a whole allocation -> naturally a noalias T[] view,
composing with vectorize.

This is a headline differentiator but a real design project (layout,
references to elements, interaction with ownership on member types).
Write its own plan before starting; do not fold into a gap-fix pass.

## G7. Smaller items

- Huge pages: MEM_LARGE_PAGES / madvise(MADV_HUGEPAGE) option in the
  vmem layer for multi-GB kernel buffers (TLB relief). Windows large
  pages need SeLockMemoryPrivilege - degrade gracefully.
- BLAS binding example DONE 2026-07-09 (agent), RE-SOURCED 2026-07-09
  (agent) to the example-local vcpkg manifest: example/vcpkg/blas_gemm.cb
  binds OpenBLAS via
  `import package-vcpkg "openblas/cblas.h" from "openblas";` (openblas
  added to example/vcpkg/vcpkg.json dependencies as
  `{ "name": "openblas", "features": ["threads"] }`; root vcpkg.json
  untouched - same pattern as the sqlite3/sdl3/zlib demos, no external SDK
  or CLI paths needed); calls cblas_dgemm (CblasRowMajor/CblasNoTrans
  straight from the header, no hand-proto) and benchmarks it against
  Mat.gemm at 256/512/1024 with a naive-triple-loop correctness check.
  example.bat picks it up automatically via the example/vcpkg/ sweep (the
  earlier %BLAS_SDK% discovery/gate block AND a second per-file
  `--c-lib libopenblas.lib` special-case in the --worker-example block
  were both removed). The vcpkg `openblas` port gates multithreading
  behind an opt-in `threads` feature (USE_THREAD, off by default) - without
  it, openblas_get_num_procs/num_threads report 1/1 regardless of host
  core count and cblas_dgemm runs single-threaded (~7 GFLOP/s @1024 on the
  dev box, slower than Mat.gemm). With `threads` on (current manifest,
  `{ "name": "openblas", "features": ["threads"] }`), num_procs/num_threads
  correctly report 20/20, but throughput (~33-39 GFLOP/s across
  256/512/1024) still lands well under the old prebuilt release's
  ~126-307 GFLOP/s and under Mat.gemm (pool, 8w)'s ~43-74 GFLOP/s.
  Root-caused (2026-07-09) and CONFIRMED CLOSED (vcpkg-side, not
  fixable from the manifest): tried adding vcpkg's `dynamic-arch` feature
  (DYNAMIC_ARCH - the prebuilt release's runtime-CPUID-dispatched,
  per-microarch assembly kernels) alongside `threads`; `vcpkg install`
  rejects it outright - the port's supports-expression restricts
  `dynamic-arch` to `!windows | mingw`, which excludes the `x64-windows`
  MSVC triplet this repo builds against. `--allow-unsupported` would force
  it past that check but the port's own message warns of known build/
  runtime problems doing so on an unsupported platform; not attempted
  without a separate, explicit approval (it would also need
  `--allow-unsupported` threaded through VcpkgResolver's `vcpkg install`
  call - a compiler change, not a manifest edit). Manifest reverted to
  `threads`-only (the verified-good config) after the attempt. The
  remaining gap to the prebuilt release's tuned kernels is therefore a
  real, currently-unclosed limitation of sourcing OpenBLAS via vcpkg's
  MSVC/x64-windows port. All of: single-thread-default, threads-feature,
  and the blocked dynamic-arch attempt are recorded honestly in
  performance/hpc/README.md "External BLAS" section alongside the
  original prebuilt-release table. Zero compiler changes made (a
  compiler change would be needed to go further, via --allow-unsupported
  plumbing).
- rdtsc-class timing: DONE (2026-07-09). time.cb already had the x86-only
  serializing rdtscp()/lfence(); the gap was a PORTABLE counter (rdtscp()
  returns 0 on non-x86, e.g. macOS arm64). Added the target-independent
  __readcyclecounter builtin (llvm.readcyclecounter; CreateReadCycleCounter
  in LLVMBackend.h, dispatch in MainListener.h) wrapped by cycle_count() and
  cycle_count_serialized() (LFENCE-bracketed on x86) in intrinsic.cb. Tests
  in Test/test_time.cb; doc in doc/LANGUAGE.md stdlib table.
- list<int[]> restriction (T[] as generic type argument does not parse):
  documented limitation; mildly hurts multi-buffer containers. Revisit
  cost/benefit - may stay a documented restriction.

---

## Constraints (repo rules that apply to all items)

- Both-pass ParseDeclarationSpecifiers for any type-syntax change (G1
  align, G3 new static methods should NOT need it - verify).
- LogError/LogErrorContext only; ASCII only; no new test files - extend
  Test/test_hpc_kernels.cb and friends.
- Gates per item: build Release, test.bat green, test_lsp.bat green if
  MainListener/LspServer touched, example.bat green; update doc/HPC.md
  in the same change.
