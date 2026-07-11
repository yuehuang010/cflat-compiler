# HPC Gaps Plan

Status: G1-G5 DONE (G2 incl Phase 2 reduce_dynamic, G4
vectorize(contract)/vectorize(reassoc) tiers, G5 incl 2026-07-10 POSIX
parity), all integrated on master, gates green. ALL of G7 is now DONE too:
rdtsc-timing and the BLAS example (2026-07-09), huge pages (2026-07-11,
Phases 1-3 - its Phase 4 and the list<int[]> restriction are the only
things left in that section). G8 (memory-mapped files - the zero-copy
INPUT side of the kernel story, and the natural producer for the noalias
span machinery G1-G4 built) is DONE as of 2026-07-11; note its benchmark
came back NEGATIVE for the single-pass streaming case, which is recorded
in full in that section and should be read before building on it. G6 (SoA
generic, a design project) is the one remaining PROPOSED item.
The former detailed sub-plans for G2/G3/G4/G5
(internal/plan/hpc-g2..g5-*.md) have been consolidated in-file below and
deleted. G7's huge-pages detail plan
(internal/plan/hpc-g7-huge-pages.md) is kept standalone: it carries the
as-built note plus the design rationale (why THP and not MAP_HUGETLB, why
macOS is a no-op tier) that the G7 bullet below only summarizes.
WSL test.sh re-verification for the core library changes deliberately
skipped (user decision at integration), except where a later item
(G5 POSIX parity, G7 huge pages) explicitly re-ran it.
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

DONE 2026-07-09 (agent, all steps including Phase 2:
parallel_reduce_dynamic<T> + _pool landed same day), integrated on
master, gates green.

Today: static balanced chunks only. Irregular work (sparse rows with
varying nnz, adaptive mesh, shrinking LU trailing update - the doc already
fights this with a work cutoff) wants dynamic/guided.

AS-BUILT: added `parallel_for_n_dynamic(_pool)` and
`parallel_reduce_dynamic(_pool)<T>` to core/hpc/parallel.cb, alongside
the existing static `parallel_for_n`/`parallel_reduce`. Tests in
Test/test_parallel.cb (33 total - not test_hpc_kernels.cb, which does
not exercise parallel.cb); benchmark performance/hpc/pfor_dynamic_bench.cb
(new file; none existed for the parallel helpers before).

Design: workers pull fixed-size chunks from a shared `atomic_counter`
(core/atomic.cb, relaxed fetch-add - the range bounds are data-dependent
on the returned value, the counter guards no other data, and worker
writes are published via the existing join/wait barrier) until
exhausted, rather than a static one-time split. Same lambda shape as
parallel_for_n (body receives [lo, hi)), so kernels migrate by changing
one call name. Default chunk size when the caller passes `chunk <= 0`:
`max(1, n / (workers * 16))` - ~16 chunks/worker bounds imbalance to
roughly 1/16 of one worker's share while amortizing atomic/dispatch
overhead; explicit `chunk` is the tuning knob for extreme per-index cost
(small) or very cheap bodies (large, to amortize counter-line
ping-pong). parallel_reduce_dynamic folds every chunk a worker pulls
into ONE local accumulator (seeded with identity, merged with combine),
written to that worker's slot; the helper merges all workers' slots at
the end exactly like the static reduce.

Deliberately not built: guided (shrinking-chunk) scheduling - the
triangular-skew benchmark below showed dynamic already near-ideal, so
guided was not warranted on the measured evidence; work-stealing
(per-worker deques) - a single shared counter is adequate at this
worker count and a different plan would be needed if a workload ever
showed the counter itself as the bottleneck.

Key implementation trap (documented for any future variant, e.g. if
guided scheduling is ever added): `ThreadPool.submit` frees its ctx via
the deleter BEFORE returning nullptr on refusal, so "drain the puller
loop then delete the box" is a use-after-free. As landed, the inline
drain on a refused submit runs off still-live locals in the helper's
own frame (body/counter/n/chunk), never the freed box.

Semantics/caveats (documented in doc/HPC.md): `parallel_for_n_dynamic`
produces results identical to serial for a race-free body (chunking
changes only which thread computes an index, not FP order within it).
`parallel_reduce_dynamic`'s chunk-to-worker assignment varies run to
run, so its FP result is NOT reproducible even at fixed worker count -
strictly weaker than the static reduce (deterministic for fixed
workers); users needing a stable result are steered to the static
variant. Tradeoff: static = zero coordination, deterministic partition,
contiguous per-worker ranges (cache/first-touch friendly), eats full
imbalance; dynamic = one relaxed atomic per chunk on a contended
counter line, non-deterministic placement, immune to skew. Rule of
thumb: uniform body -> static; skewed body -> dynamic.

Measured (4 workers, triangular skew, n=65536): serial 1463ms, static
639ms (2.29x), dynamic 363ms (4.03x - near-ideal). Uniform body:
comparable at moderate n, dynamic ~23% slower at n=2^22 (counter-line
contention) - documented tradeoff, not a bug.

WSL test.sh re-verification skipped at integration (user decision).

## G3. SIMD completeness

DONE 2026-07-09 (agent, all 3 phases), integrated on master, gates
green: test.bat all pass, test_lsp 201/0, example.bat 88/0/24.

Today: no lane writes, no shuffles (doc says so), no horizontal reduce,
no masked load/store, no gather/scatter.

AS-BUILT: added `reduce_add`/`reduce_min`/`reduce_max`, `lanes()`, `load_masked`/
`store_masked`, and `load_gather`/`store_scatter` to `simd<T,N>`, all as
new branches in the existing `ParseSimdStaticMethod` dispatch
(MainListener.h) - no grammar or ParseDeclarationSpecifiers changes
(verified, per the G3 constraint from the parent plan). Each op is a
1:1 lowering to a portable LLVM intrinsic (`llvm.vector.reduce.*`,
`llvm.masked.*`, never `llvm.x86.*`, per internal/simd-type.md). Tests:
test_hpc.cb grew from 141 to 170 asserts, plus 4 new
Test/errors/err_simd_*.cb. -O2 spot check matches -O0.

Design decisions:
- `reduce_add` on float lanes is REASSOCIATING
  (`llvm.vector.reduce.fadd` with the `reassoc` flag, start = +0.0),
  matching the vectorize contract's documented reduction semantics; no
  ordered variant was added. `reduce_min`/`reduce_max` use
  `llvm.vector.reduce.fmin`/`fmax` for float (minnum/maxnum NaN
  semantics, consistent with the existing min/max vector math) and
  smin/umin/smax/umax by signedness for int. bool (i1) lanes are
  rejected with an error (use select/mask ops instead).
- `lanes()` (constant iota via ConstantVector) turned out to be
  REQUIRED, not optional - there is no other way to build a tail mask
  (compare a lane-index vector against a splat of the remaining count),
  so it shipped as part of the masked-load/store phase rather than
  being deferred.
- Gather/scatter index validation checks the LLVM element type
  (`isIntegerTy(32)`/`isIntegerTy(64)`), not type-name strings, so all
  int aliases (int/i32/u32/long/i64/u64) are accepted uniformly.
  Gather/scatter shipped unmasked-only (MVP; mask operand = all-true
  splat, poison/undef passthru on gather) - masked overloads deferred
  until a kernel needs them. Scatter with duplicate indices stores in
  lane order (last lane wins) - LLVM's defined behavior, documented so
  users are not surprised.
- Intrinsic overload signatures were verified against vcpkg LLVM 18
  (not guessed): `vector.reduce.fadd` overloads on `{vecTy}` only
  (start operand supplied separately); `masked.load`/`store` overload
  on `{vecTy, ptrTy}`; `masked.gather`/`scatter` overload on
  `{vecTy, ptrVecTy}` and take `<N x ptr>` + i32 align + mask (+
  passthru on loads/gathers).

Explicitly NOT done, deferred to a future plan when a kernel actually
needs them: shuffles/permutes, lane writes (`v[i] = x` stays an error),
`load_aligned` variants (belong with G1's alignment follow-on).

Known cleanup identified but not fixed: the new branches return
immediately after `LogErrorContext` on arity/shape mismatch, but the
pre-existing load/store/select/clamp branches log-and-continue and then
index `argNVs[k]` unconditionally - a latent OOB read on malformed
calls to those older ops.

LSP: no hardcoded simd method list exists anywhere (completion is
generic), so nothing needed extending there; test_lsp.bat was still run
since MainListener.h was touched.

## G4. Scoped FP-math control (FMA contraction + reassociation)

LANDED 2026-07-09 (Windows), integrated on master, gates green.
RE-SCOPED by user decision: no new `fastmath` keyword - extended the
existing `vectorize` soft keyword to `vectorize(contract)` and
`vectorize(reassoc)` (two tiers, reassoc implies contract - not
composable flags; lock-clause grammar precedent; builder-default FMF
save/set/restore around the loop body brackets). Serial-FMA escape
hatch already existed and was left alone (scalar `fma()` in
core/math.cb + simd fma, both single-rounding `llvm.fma`).

Today: `vectorize` on a reduction is opt-in reassociation for that loop
(tasteful partial answer). But there is no spelling anywhere for FMA
*contraction* (a*b+c -> one vfmadd when the target has FMA; distinct
from simd.fma's single-rounding semantics) or for reassociation outside
a vectorize loop. With --cpu native, contraction alone is a large
fraction of peak FLOPs.

AS-BUILT (files changed): `cflat/CFlat.g4` (`vectorizeStatement` gains
`('(' Identifier ')')?`); `cflat/MainListener.h`
(`enum class VectorizeFpTier {None,Contract,Reassoc}` +
`vectorizeFpTier_` flag + `ApplyVectorizeFpTier` helper; parse-time
validation accepts `contract`/`reassoc` else `LogErrorContext` + return;
tier captured/cleared alongside `vectorizeActive_`; FMF
save/set/restore wrapped around `ParseControlledBody` at both the
while-form and for-form body brackets); `Test/test_vectorize.cb` (4 new
cases: contract approx+exact, reassoc approx+exact);
`Test/errors/err_vectorize_bad_flag.cb` (new); `doc/HPC.md`
(FP-math-tiers subsection + reduction note + fma cross-link).

Semantics: `vectorize(contract)` sets only the `contract` FMF (fuses
mul+add into fma where the target has it; never reorders across
statements - the "safe" tier). `vectorize(reassoc)` sets `reassoc` AND
`contract` (extends the reordering plain `vectorize` already grants the
reduction chain to ALL FP math in the lexical body - results can differ
from serial by more than reduction ULP drift; documented as the
"throughput over reproducibility" tier). Region = the lexical body
under `ParseControlledBody`, including nested non-tiered loops (which
inherit the tier) and lambdas defined inside the region (which also
inherit it, since lambda-body emission rides the same builder and
`BuilderState` does not snapshot FMF - a deliberate, documented
lexical-scope semantics, not a leak: the FMF is restored immediately
after `ParseControlledBody` so nothing outside the loop is affected).

Two open questions were resolved empirically rather than assumed
(recorded so they are not re-litigated): (1) lambdas inside a tiered
body DO inherit the tier - verified in `.ll` (nested plain loop inside
a tiered loop shows the inner body carrying `contract`; a tiered loop
inside a plain loop shows only the inner body carrying it, with the
outer loop's ops flagless before and after - balanced save/restore).
(2) `reassoc` does NOT also need `nsz` - empirically, `vectorize(reassoc)`
on a sum-of-products at `-O2 --cpu x86-64-v3` already splits into four
independent accumulators and finishes with `llvm.vector.reduce.fadd`,
carrying `reassoc contract` with no `nsz` (LLVM 18 initializes
split-lane identities with `-0.0` itself, so it does not need the
source FMF to carry nsz).

Measured/verified: unoptimized `.ll` shows `fmul`/`fadd` carrying
exactly the expected flags (contract-only vs reassoc+contract vs bare,
correctly scoped). At `-O2 --cpu x86-64-v3`, a contract mul-add kernel
vectorizes to `<4 x double>` and `llc` lowers it to
`vfmadd132pd`/`vfmadd132sd`, vs 0 vfmadd/0 contract flags for the
identical kernel under plain `vectorize` - confirming the tier is what
enables fusion. Note: `--cpu native` resolved to baseline SSE2 (no FMA)
on the dev box at measurement time, so `x86-64-v3` was needed to see
the fmadd win - `--cpu native`'s actual FMA availability is
host-dependent, not a fixed compiler fact.

Out of scope, deferred until asked for: `vectorize(fast)` and
fine-grained flags (nsz/arcp/afn, especially nnan/ninf which turn data
conditions into poison), a whole-program `--fp-contract` CLI flag,
contraction outside vectorize loops.

Gates (Release, all green): build + ANTLR regen; `test.bat` all pass
(incl. test_vectorize + the new error test); `test_hpc.bat` all pass
(test_vectorize `--run -O2` positive + three vectorize negatives +
span-noalias); `test_lsp.bat` 202/0 (one run showed 4 transient
`example/vcpkg/*` failures from a cold header-bind cache under the
parallel sweep - a warm-cache re-run was 202/0 and the baseline exe
passed them too, not a regression); `example.bat` 88/0/24.
Perf-evidence benchmarking beyond the `.ll`/asm spot checks above was
left to the main session (skipped in this pass per instructions).
Both-pass ParseDeclarationSpecifiers and the vectorize
enforcement/loop-metadata machinery (LLVMBackend.cpp) were confirmed
untouched.

## G5. Topology-aware CPU masks

DONE 2026-07-09 (agent, Windows), extended to POSIX PARITY DONE
2026-07-10 (agents, Linux + macOS). Pure library change in both phases
- no compiler/grammar work.

Today: cpu_mask_lowest(n) assumes cores 0..n-1 are the right set. On
hybrid parts that is wrong by default: on the dev box (Strix Point,
4x Zen5 + 6x Zen5c, split CCX), cpu_mask_lowest(8) spans both core types
and both CCXs - exactly the jitter pinning exists to remove.

AS-BUILT (Windows, 2026-07-09): new core/topology.cb + os.windows.cb
externs (GetLogicalProcessorInformationEx, GetActiveProcessorCount). A
lazily-initialized, mutex-memoized CpuTopology snapshot walks the Ex
buffer in one RelationAll pass (RelationProcessorCore -> per-core mask
+ EfficiencyClass; RelationCache level==3 -> LLC domains, deduped;
RelationNumaNode -> NUMA domains), exposing cpu_mask_physical(n) /
cpu_mask_perf_cores() / cpu_mask_efficiency_cores() / cpu_mask_llc(domain)
/ cpu_llc_count() / cpu_mask_numa(node) / cpu_numa_count() on top. The
u64-mask contract end to end (ThreadPool.init pinMask ->
Thread.setAffinity -> os.thread_set_affinity -> SetThreadAffinityMask)
is untouched - G5 only adds smarter mask CONSTRUCTORS. cpu_mask_lowest
stays as-is for the simple case. Scoped to group-0 / <=64 logical
processors (documented limit, same one cpu_mask_lowest already had
implicitly). Windows-only by original design (POSIX had a no-op
thread_set_affinity, so topology was judged useless there at the time -
superseded 2026-07-10, see below).

Offsets (RelationProcessorCore/RelationNumaNode mask at buffer+32,
RelationCache(L3) mask at buffer+40) were read directly off the
installed Windows SDK header (not guessed) and confirmed by a
throwaway smoke program against the dev box's known topology. Test:
testCpuTopology() in Test/test_threadpool.cb, machine-independent
assertions (physicalCount in [1,logicalCount], llcMask
pairwise-disjoint and covering, cpu_mask_physical(n) popcount ==
min(n,physicalCount), perf|efficiency covers all physical cores,
perf&efficiency==0).

Measured on the dev box (AMD Ryzen AI 9 365 / Strix Point, 20
logical/10 physical, 4x Zen5 + 6x Zen5c): logicalCount=20
physicalCount=10 llcCount=2 numaCount=1; EfficiencyClass DOES
distinguish the two core types (Zen5 cores 0-3 = class 1, Zen5c cores
4-9 = class 0), and on this part the class split happens to coincide
exactly with the LLC/CCX split (cpu_mask_perf_cores()==cpu_mask_llc(0)==
0xff, cpu_mask_efficiency_cores()==cpu_mask_llc(1)==0xfff00) - not
guaranteed on other hybrid parts, just observed here. Benchmark
performance/hpc/topology_pingpong_bench.cb (cache-line ping-pong
latency, pinned via cpu_mask_llc(), best-of-5 fresh-process runs):
cross-CCX line bounce is consistently ~1.8-2.2x slower than either
intra-CCX case (~90ns intra-CCX0/Zen5, ~105ns intra-CCX1/Zen5c, ~190ns
cross-CCX), directionally consistent with (not expected to numerically
match) the ~2.8x one-way L3-latency figure from perf_cache_mountain.cb
- confirming cpu_mask_llc() tracks a real hardware boundary.

POSIX PARITY DONE 2026-07-10 (agents): the original Windows-only scope
was revisited and extended so all three platforms do a real topology
walk.
- Linux: sysfs-based (cpu/online for the actual online set - NOT
  0..nproc-1, which is wrong under isolcpus/offlined cores;
  thread_siblings_list collapses SMT; cache/indexN highest level >=2
  treated as LLC; node/online + node/nodeN/cpulist for sparse NUMA
  ids). coreEff approximated from ARM cpu_capacity rank, else Intel
  cpu_core/cpu_atom, else uniform 0. Affinity is REAL via
  pthread_setaffinity_np (128-byte cpu_set_t) - unlike the original
  Windows-only plan's assumption, Linux affinity is not a no-op.
- macOS: sysctlbyname-based (hw.physicalcpu / hw.logicalcpu /
  hw.nperflevels / hw.perflevelN.{physicalcpu,logicalcpu,cpusperl2});
  perflevel 0 is the MOST performant so coreEff = nperflevels-1-level;
  LLC domain = each perflevel's cpusperl2 cluster. Mask bits are
  SYNTHETIC (perflevel order, no OS-visible CPU numbering). Affinity is
  NONE - Darwin has no CPU affinity API (thread_affinity_policy returns
  KERN_NOT_SUPPORTED on Apple Silicon); thread_set_affinity is an
  honest no-op and os.thread_affinity_supported() lets callers branch.
  The actionable lever instead is QoS: cpu_qos_for_mask(mask) maps to
  QOS_CLASS_BACKGROUND (subset of eff cores) / USER_INTERACTIVE (subset
  of perf cores) / 0 otherwise, INCLUDING on any uniform
  single-core-class machine, where there is no P/E split to express and
  promoting QoS would be an unrequested side effect. ThreadPool workers
  call os.thread_set_qos_self() on entry when pinMask != 0 (QoS is
  self-only on Darwin).

Verified on an Apple Silicon box (18 logical = 6 P + 12 E, 2
perflevels): snapshot exactly matches sysctl - physicalCount=18,
llcCount=3 (P cluster + two 6-core E L2 clusters), perf=0x3f,
eff=0x3ffc0, qos(perf)=0x21, qos(eff)=0x09. test_threadpool REMOVED
from test.sh's SKIP list and now runs green (test.sh Release: 158
passed / 0 failed / 17 skipped, up from 157/0/18). All four targets
(win64/win32/linux/macos) type-check and code-gen via --platform
cross-targeting from macOS.

NOT VERIFIED as of 2026-07-10: the Linux path has never actually been
RUN (no Linux host or container runtime available in that session); it
is compile- and codegen-checked only, plus its cpulist parser was
extracted and unit-exercised natively against real sysfs strings
(ranges, sparse lists, empty, reversed, >=64 clamp). Windows was not
rebuilt in that session; its code path is semantically unchanged
(mechanical _topo_build_windows -> _topo_build rename +
_topo_llc_mask_seen hoisted to file scope) and still
type-checks/codegens for win64+win32. Re-run test.bat on Windows and
test.sh on WSL before trusting either path in anger.

WSL test.sh re-verification for the original Windows-only core library
change was skipped at 2026-07-09 integration time (standing user
decision) - effectively superseded by the actual test.sh run during the
2026-07-10 POSIX work above.

Natural landing spot later for per-node arenas if NUMA hardware ever
matters (deferred - was single-node-only evidence at the time of the
Windows phase; CCX effects were testable there, node placement was
not). No guided placement policy was added to the parallel helpers
(e.g. auto-pin to perf cores) - masks stay an explicit user decision.

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

- Huge pages: DONE 2026-07-11 (agent, Phases 1-3). Detail plan (kept
  standalone, carries the as-built note): internal/plan/hpc-g7-huge-pages.md.
  MEM_LARGE_PAGES / madvise(MADV_HUGEPAGE) in the vmem layer for multi-GB
  kernel buffers (TLB relief), degrading gracefully everywhere.
  AS-BUILT: os.huge_page_bytes()/huge_pages_enable() plus a single
  os.vm_reserve_commit_ex(size, node, huge) that vm_reserve_commit and
  vm_reserve_commit_numa are now thin wrappers over (core/os.cb);
  vmem_huge_pages_available/huge_page_bytes/alloc_huge/alloc_numa_huge
  (core/vmem.cb), freed by the EXISTING vmem_free/vmem_free_numa - the
  header-stash release is symmetric for huge and normal regions alike.
  Windows: MEM_LARGE_PAGES + the SeLockMemoryPrivilege token dance
  (re-checks GetLastError, since AdjustTokenPrivileges returns TRUE even
  when it grants nothing), size rounded to GetLargePageMinimum(), one
  retry without MEM_LARGE_PAGES on failure (fragmentation can deny large
  pages even with the privilege held). Linux: transparent huge pages via
  madvise(MADV_HUGEPAGE) on a 2 MB-aligned mapping - deliberately NOT
  MAP_HUGETLB, which hard-fails unless an admin preallocated
  vm.nr_hugepages and so is a bad default for a library. macOS: honest
  no-op tier (VM_FLAGS_SUPERPAGE_SIZE_2MB is x86-only and dead on Apple
  Silicon), same posture as NUMA binding and affinity.
  Invariant held: _vm_reserve_commit_core's huge=false path is
  byte-identical to before (64 KB granule, same 16-byte [pad,total]
  header) - it sits behind every POSIX vmem_alloc and every allocator
  depends on its 64 KB alignment (BucketAllocator masks ptr & ~0xFFFF).
  Deviation from plan: the privilege APIs need advapi32.lib, which was not
  in the default Windows link list - added to LLVMBackend.h and both
  synthetic-import-lib generators in LLVMBackend.cpp (mechanical, mirrors
  the kernel32/ws2_32/ntdll/dbghelp entries). So this was NOT the
  compiler-free change the plan predicted.
  Tests: graceful-degrade asserts in Test/test_threadpool.cb beside the
  existing vmem_alloc_numa block. Benchmark:
  performance/hpc/hugepage_bench.cb (dependent 2 MB-strided pointer chase -
  TLB-bound, not bandwidth-bound).
  NOT VERIFIED: the huge-vs-normal win itself. The dev box does not hold
  SeLockMemoryPrivilege, so the benchmark ran as a self-labelled A/A
  (~36.0 ns/access both ways, vmem_huge_pages_available()==false). What
  IS proven here is the degrade path. Re-run on a privileged box (or a
  Linux host with THP in madvise/always mode) before claiming a TLB win.
  Deferred: Phase 4 (huge-backed segments for arena_allocator and the
  core/hpc kernel buffers) - deliberately gated on that missing evidence.
  Gates green: test.bat 45/45, test_lsp.bat 204/0, example.bat 89/0/24,
  WSL test.sh 159/0/16.
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

## G8. Memory-mapped files (mmap-with-fd / MapViewOfFile)

Status: DONE (2026-07-11, all 3 phases). Its detail plan
(internal/plan/hpc-g8-mapped-files.md) has been consolidated into this
section and deleted, the same way G2-G5's were. It WAS a pure
core-library change - no compiler, grammar, or codegen work (unlike G7,
which broke that same claim on advapi32.lib): every API used is kernel32
or libc, both already in the default link set.

The API shipped as the plan sketched it: `asSpan<T>` is an INSTANCE method,
called `m->asSpan<double>()`. It briefly shipped as a static taking the
receiver explicitly, because a generic method with its own type parameter
on a non-generic class could not resolve `this` or its fields - it failed
with "Undefined variable <field>". That turned out to be a real compiler
bug rather than a design constraint: it was diagnosed and FIXED the same
day (cflat/MainListener.h - generic member methods are now registered as
owner-keyed templates and the monomorphized body is emitted as a true
member with its implicit `this`; regression coverage in
Test/test_generics.cb), and asSpan was moved back to the natural spelling.
So this DID end up touching the compiler after all - but for a bug the
feature merely exposed, not for anything mapped files needed.

Still open, found while fixing that: a STATIC generic method on a GENERIC
class is unreachable through the mangled type - see
internal/issue/static-generic-method-on-generic-class.md. Pre-existing,
unrelated to mapped files.

Landed: os.map_file/unmap_file/flush_view in core/os.cb (zero-length and
failure both collapsed into this one place: empty file -> nullptr with
*outLen == 0 and SUCCESS, real failure -> nullptr with *outLen == -1);
new Windows externs (CreateFileA, GetFileSizeEx, SetFilePointerEx,
SetEndOfFile, CreateFileMappingA, MapViewOfFile, UnmapViewOfFile,
FlushViewOfFile) and POSIX externs (open, ftruncate, lseek, msync);
owning MappedFile in core/filesystem.cb. Two implementation notes worth
keeping: SetFilePointerEx was NOT in the plan's extern list but is
required (SetEndOfFile truncates to the CURRENT file pointer, so the
pointer must be positioned first), and lseek(SEEK_END) is used instead of
fstat to read a file's size, which dodges hand-deriving st_size's offset
(it differs between glibc and Darwin).

BENCHMARK RESULT IS NEGATIVE, AND THAT IS THE POINT OF HAVING RUN IT.
performance/hpc/mmap_scan_bench.cb, 256 MB of doubles, warm page cache,
best-of-5, both variants feeding the SAME vectorize(reassoc) kernel:

    File.readBytes + sum       59.92 ms
    MappedFile.asSpan + sum    80.86 ms      (sums bit-identical)

The copy variant WON. First-touch soft page faults on a freshly-mapped
256 MB region (~64K faults) cost more than one bulk read plus a
memcpy-speed scan. So: do NOT reach for MappedFile expecting a
single-streaming-pass win - the user-space copy was not the bottleneck.
Where it is still expected to pay: repeated reads of the same file,
random access, and avoiding a re-copy across multiple passes. Anyone
building on this should re-measure their own shape rather than inherit
the assumption. This also sharpens the follow-on: the lever for mappings
is the advice/prefetch APIs (madvise MADV_WILLNEED/MADV_SEQUENTIAL,
PrefetchVirtualMemory), aimed squarely at that fault cost.

SPUN OFF - a doc inaccuracy this work exposed, NOT fixed here: doc/HPC.md's
"FP-math tiers" section reads as though a plain `vectorize` will vectorize a
floating-point reduction. It will not - LLVM rejects it ("not a recognized
reduction"), and the sum kernel needs `vectorize(reassoc)`. Confirmed with a
minimal probe while writing the benchmark. Harmless but misleading; the
wording should be tightened in its own pass.

Gates: test.bat 45/45 (test_filesystem 88/88, +14 new MappedFile
assertions covering round-trip, empty file, missing file, asSpan values,
and a 2000-iteration map/drop loop); example.bat 89/0/24; WSL test.sh
159/0/16. test_lsp.bat not run - no compiler file touched.

Caveat on the Linux coverage: test_filesystem.cb is on test.sh's
PRE-EXISTING skip list (it assumes Windows backslash paths - unrelated to
this work), so the new MappedFile assertions do NOT run in that 159/0/16.
MappedFile was instead verified against the Linux ELF cflat with a
standalone probe (round-trip + empty-file case). The POSIX real-fd mmap
path is therefore proven, but it is NOT yet guarded by the automated
suite - if test_filesystem.cb is ever made path-portable, that skip should
be lifted and this becomes moot.

One weak assertion, recorded rather than silently inherited: the
"map/drop loop does not leak" test maps 2000 x 64 KB (~131 MB total). On
64-bit that would not exhaust address space even if the destructor never
unmapped, so it would pass with a leak present. The destructor path is
simple enough that this was judged not worth hardening; anyone touching
MappedFile's lifetime should not treat that test as a real guard.

Today: cflat has NO file-mapping binding at all. The `mmap` extern in
core/os.posix.cb is only ever called anonymously (fd = -1,
MAP_ANON_PRIVATE) as the allocator substrate behind vmem_alloc, and
Windows has no CreateFileMapping/MapViewOfFile extern whatsoever.
core/filesystem.cb's `File` is stdio (FILE*), so reading an input means a
full copy through user space - for a multi-GB dataset, that copy is the
first thing standing between the program and the kernel.

Proposal: neutral os.map_file/unmap_file/flush_view primitives in
core/os.cb (the vmem/NUMA layering precedent), and an owning `MappedFile`
class in core/filesystem.cb shaped like VMemRegion (unmaps in the
destructor). NOT an extension of `File` - that class is FILE*-based and a
mapping needs raw handles.

Why it belongs in THIS plan rather than being a filesystem chore: a
mapping is the natural producer for the noalias machinery G1-G4 built.
`MappedFile.asSpan<T>()` hands the region out as a first-class `span<T>`
that feeds vectorize/parallel_for_n with zero copy. The `(T[])` unsafe
escape it uses internally is SOUND here rather than merely trusted - a
mapping IS a whole, distinct VM region by construction, which is exactly
what the cast asserts - so the unsafe stays confined to one reviewed line
inside core and callers never see a raw pointer.

Key facts (detail plan has the rest): the mapping outlives the handles
that made it on BOTH platforms (POSIX close(fd) after mmap; Windows
CloseHandle both after MapViewOfFile), so MappedFile stores only
{addr, len} and the platforms converge on one representation. Windows
CANNOT map a zero-length file; a write mapping cannot grow the file (size
is fixed before mapping); truncation under a live mapping is a hard fault
(SIGBUS / EXCEPTION_IN_PAGE_ERROR), not an error code.

G7 and G8 do NOT compose: huge pages do not back mapped file views on
either platform (Windows SEC_LARGE_PAGES is pagefile-backed sections only;
Linux THP does not back file mappings in the general case). The real lever
for a mapping is madvise(MADV_WILLNEED/MADV_SEQUENTIAL) /
PrefetchVirtualMemory - a follow-on item, and only once the benchmark
shows where the time goes.

Verification: extend Test/test_filesystem.cb (no new test file);
benchmark performance/hpc/mmap_scan_bench.cb (readBytes-into-heap vs
mapped span<double> through a vectorize loop). Re-verify on WSL - this is
the first real-fd mmap call on the POSIX side.

---

## Constraints (repo rules that apply to all items)

- Both-pass ParseDeclarationSpecifiers for any type-syntax change (G1
  align, G3 new static methods should NOT need it - verify).
- LogError/LogErrorContext only; ASCII only; no new test files - extend
  Test/test_hpc_kernels.cb and friends.
- Gates per item: build Release, test.bat green, test_lsp.bat green if
  MainListener/LspServer touched, example.bat green; update doc/HPC.md
  in the same change.
