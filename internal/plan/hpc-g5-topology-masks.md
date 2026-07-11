# G5: Topology-Aware CPU Masks - Detailed Plan

Status: IMPLEMENTED (steps 1-4 landed; bench step 5 deferred to the main session)
Created: 2026-07-09
Parent: internal/plan/hpc-gaps.md (G5)

Goal: replace "cpu_mask_lowest(n) and hope cores 0..n-1 are the right
ones" with mask constructors that understand the machine: physical vs SMT,
performance vs efficiency cores, and LLC/CCX domains. On the dev box
(Strix Point: 4x Zen5 + 6x Zen5c on separate CCXs, ~2.8x L3 latency gap),
cpu_mask_lowest(8) spans both core classes and both CCXs - exactly the
jitter pinning exists to remove.

## Is this a library change?

YES - pure library. The mask contract is a flat `u64` end to end
(ThreadPool.init pinMask -> __threadpool_nth_core -> Thread.setAffinity ->
os.thread_set_affinity -> SetThreadAffinityMask), so G5 only adds smarter
mask CONSTRUCTORS; every consumer stays byte-identical. No compiler,
grammar, or ParseDeclarationSpecifiers work. No build plumbing is needed for
the new core file - CMake's `CONFIGURE_DEPENDS` glob deploys everything under
`cflat/core/` automatically.

## Scope decision: Windows-only (deliberate)

os.cb:563's POSIX branch of thread_set_affinity is an explicit no-op
("POSIX: not ported"), and os.posix.cb has zero affinity/topology code.
Topology-aware masks are useless on a platform where setAffinity does
nothing, so POSIX (pthread_setaffinity_np + sysfs) is OUT OF SCOPE here -
it belongs to a future "port affinity to POSIX" item that would carry its
own topology walk. The new core file still compiles on POSIX: gate with
`if const (__WINDOWS__)` and have the else-branch degrade to the
cpu_mask_lowest equivalents (documented), same shape as os.cb does today.

Also out of scope: >64 logical processors / Windows processor groups. The
u64 mask covers group 0 only; the walk reads GROUP_AFFINITY from group 0
and ignores others. Document the limit next to the helpers (same limit
cpu_mask_lowest already has implicitly).

---

## What exists today (survey 2026-07-09)

- Mask type: bare u64. `cpu_mask_lowest(int n)` + internal
  `__threadpool_nth_core(mask, idx)` live in core/threadpool.cb:274/286.
- Consumers: ThreadPool.init(workerCount, pinMask, fpConfig)
  (threadpool.cb:380, re-applied in resize :650); parallel_for_n family's
  `pin` pins worker w to `1 << (w % hardware_concurrency())`
  (hpc/parallel.cb:148).
- Win32 externs in core/os.windows.cb: SetThreadAffinityMask (:32),
  GetSystemInfo (:126), GetLogicalProcessorInformation NON-Ex (:128).
  NOT bound yet: GetLogicalProcessorInformationEx, GetActiveProcessorCount.
- In-core precedent for a double-call+walk of a Win32 buffer:
  cache_line_size() in os.cb:152 (non-Ex, fixed stride).
- The REAL prototype: performance/perf_cache_mountain.cb:44-105 already
  declares GetLogicalProcessorInformationEx locally and walks the
  variable-stride Ex records (Size at +4, EfficiencyClass at +9,
  GROUP_AFFINITY mask at +32) to enumerate physical cores + core class.
  G5 = lift this into core properly, add the RelationCache(L3) and
  RelationNumaNode passes, and expose mask helpers.
- Counting: hardware_concurrency() = logical count only (GetSystemInfo).
  No physical/SMT/LLC/NUMA distinction exists in core.

---

## Design

### New core file: core/topology.cb (explicit import, like the rest)

One lazily-computed, memoized snapshot per process (topology cannot change
mid-run for our purposes; a static owning instance + init flag, no locking
needed beyond first-touch from main - document "query once from the main
thread before spawning workers" OR guard with the existing mutex pattern -
pick the mutex guard, it is cheap and removes the footgun).

Data model (fixed-capacity, no allocation surprises; 64 lanes max by the
u64 decision):

```cflat
struct CpuTopology {
    int  logicalCount   = 0;   // popcount of group-0 active mask
    int  physicalCount  = 0;   // # RelationProcessorCore records
    int  llcCount       = 0;   // # RelationCache level-3 records (CCX count)
    int  numaCount      = 0;   // # RelationNumaNode records
    u64[64] coreMask    = default;  // per physical core: mask of its SMT siblings
    u8[64]  coreEff     = default;  // per physical core: EfficiencyClass
    u64[8]  llcMask     = default;  // per LLC domain: mask of member logical CPUs
    u64[8]  numaMask    = default;  // per NUMA node
}
CpuTopology* cpu_topology();   // memoized; never returns nullptr
```

(Exact field spelling/containers to match core style at implementation
time; the point is fixed arrays indexed 0..count-1, masks as u64.)

### Mask helpers (the actual G5 API, alongside the struct)

```cflat
u64 cpu_mask_physical(int n);     // one logical CPU per physical core,
                                  // lowest SMT sibling of the first n cores
u64 cpu_mask_perf_cores();        // all logical CPUs whose physical core has
                                  // the MAX EfficiencyClass present
u64 cpu_mask_efficiency_cores();  // complement within physical cores: all
                                  // cores below the max class (empty if uniform)
u64 cpu_mask_llc(int domain);     // all logical CPUs sharing LLC #domain
int cpu_llc_count();              // how many domains exist (loop bound)
u64 cpu_mask_numa(int node);      // same shape for NUMA
int cpu_numa_count();
```

Decisions (settled here so implementation does not re-litigate):
- EfficiencyClass semantics: HIGHER value = more performant class (Win32
  contract). cpu_mask_perf_cores = cores at the max class observed. If all
  cores report one class (uniform machine, and possibly Strix Point - AMD
  may report Zen5/Zen5c identically; VERIFY on the dev box during
  implementation), perf_cores == all physical cores' logical CPUs and
  efficiency_cores == 0. That is correct-by-definition, not a failure;
  document that on such machines cpu_mask_llc is the discriminating tool
  (the CCX split IS observable via RelationCache L3 regardless of class).
- Degradation: if GetLogicalProcessorInformationEx fails or the walk finds
  nothing (odd VMs), populate the snapshot from GetSystemInfo alone:
  physicalCount = logicalCount, coreMask[i] = 1<<i, eff uniform, one LLC =
  one NUMA = full mask. Helpers then behave like cpu_mask_lowest - never
  return 0 masks that would silently un-pin a pool.
- Ordering: cpu_mask_physical(n) takes cores in record order (which is
  enumeration order, in practice grouped by CCX on AMD) - do NOT sort by
  EfficiencyClass; composing class + count is what
  `cpu_mask_perf_cores()` then popcount/nth-bit is for. Keep each helper
  single-purpose.
- The helpers live in topology.cb, NOT threadpool.cb, so topology is
  usable without a pool; threadpool.cb keeps cpu_mask_lowest untouched
  (it remains the right tool when the caller has already decided the CPU
  set, and the POSIX/doc story for it is unchanged).

### Windows implementation

1. New externs in core/os.windows.cb (namespace os.windows, style of :126):
   `GetLogicalProcessorInformationEx(u32 relationship, i8* buf, u32* len)`
   and `GetActiveProcessorCount(u16 group)` (0xFFFF = all groups; use for
   the >64-LP detection note). Raw-byte buffer + offset walk, exactly like
   perf_cache_mountain.cb:66 - do NOT try to model the full
   SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX union as a cflat struct; the
   variable-length GROUP_AFFINITY arrays make offset arithmetic the honest
   representation. Offsets to use (validated by the existing benchmark):
   Relationship at +0 (u32), Size at +4 (u32), and for
   RelationProcessorCore: Flags at +8 (u8, LTP_PC_SMT), EfficiencyClass at
   +9 (u8), GroupCount at +26? - NO: just GROUP_AFFINITY[0].Mask at +32
   (u64) as the benchmark does, reading group 0 only. For RelationCache:
   Level at +8+? - the CACHE_RELATIONSHIP layout differs; verify offsets
   against the SDK header at implementation time and comment each offset
   with the field name (the benchmark comments are the pattern).
2. Three passes over the same buffer (or one pass with a switch):
   RelationProcessorCore (relationship 0) -> coreMask/coreEff;
   RelationCache (2), keep only Level==3 -> llcMask (dedupe identical
   masks - L3 records can repeat per subunit); RelationNumaNode (1) ->
   numaMask. Call once with relationship=RelationAll (0xFFFF) to get all
   record types in one buffer.
3. Memoize under a lazily-initialized mutex (core has the lazy
   pthread/SRW mutex pattern already; copy it).

### POSIX branch (compiled, degraded)

`if const (__WINDOWS__)` around the walk; else-branch = the degradation
path above (logical count from sysconf via the existing os.num_processors,
uniform synthetic topology). No new POSIX externs. Comment points at the
future affinity-port item.

---

## What this deliberately does NOT change

- ThreadPool.init / parallel_for_n signatures: untouched. The pin
  machinery already accepts any u64; only mask construction gets smarter
  (this is the G5 contract from the parent plan).
- cpu_mask_lowest: kept as-is, still documented for the simple case.
- No per-NUMA-node arenas / allocation placement (single-node dev box;
  parent plan defers this explicitly).
- No guided placement policy in the parallel helpers (e.g. auto-pin to
  perf cores) - masks stay an explicit user decision; a default-policy
  change would need its own discussion.

## Implementation steps

1. os.windows.cb externs + core/topology.cb walk + CpuTopology snapshot +
   degradation path. No build plumbing needed - CMake's core-deploy glob
   picks up new core/*.cb files automatically. REBUILD BEFORE TESTING -
   deployed-copy trap.
2. Mask helpers on the snapshot.
3. Tests: extend Test/test_threadpool.cb testPinnedPool area (no new test
   files): snapshot sanity (physicalCount>=1, <=logicalCount; llcCount>=1;
   every llcMask non-zero, pairwise-disjoint, union covers all cores'
   masks; cpu_mask_physical(n) popcount == min(n, physicalCount); perf |
   efficiency == all-physical union; perf & efficiency == 0). These
   assertions are machine-independent. Affinity itself remains
   unobservable from cflat (existing test comment) - helper math only.
4. doc/HPC.md "Pinning workers to cores" section: replace the hand-built
   P-core-mask advice (:733) with the new helpers; add a Strix-Point-style
   worked example (pin a pool inside one CCX via cpu_mask_llc(0)); state
   the 64-LP/group-0 limit and the POSIX no-op status. doc/THREADING.md
   pool section gets the helper names.
5. Bench evidence on the dev box: intra-CCX vs cross-CCX pinned
   producer/consumer (or adapt perf_cache_mountain) showing the known ~3x
   line-transfer gap; record whether EfficiencyClass distinguishes
   Zen5/Zen5c here (feeds the doc wording for perf_cores on AMD).
   performance.bat with -O2, pinned, fresh processes per variant (hybrid
   dev box measurement rules from memory).

## Verification gates

- Build Release; test.bat green; example.bat green. test_lsp.bat NOT
  needed (no MainListener/LspServer change) unless step 1 ends up touching
  the compiler after all (it should not).
- No WSL run needed: POSIX branch is a compile-only degradation path, but
  it must still COMPILE on POSIX - keep the if-const shape mirrored from
  os.cb so the existing porting story holds. (test.sh re-verification
  stays skipped per the standing user decision unless asked.)
- Mask-helper assertions from step 3 pass on the dev box AND would pass on
  a uniform machine (write them machine-independent).

## Sequencing / delegation

Step 1 is the only subtle part (offset walk, dedupe, memoization) -
sonnet with this plan plus perf_cache_mountain.cb:44-105 as the reference;
escalate to opus on flail. Steps 2-5 are mechanical on top. Independent of
G2 Phase 2 (disjoint files: topology.cb/os.windows.cb vs hpc/parallel.cb) -
safe to run in parallel in its own worktree if desired.

## Implementation notes (as landed)

**Offsets used** - verified against the real Windows SDK header on the dev
box (`C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\winnt.h`,
`_PROCESSOR_RELATIONSHIP` / `_NUMA_NODE_RELATIONSHIP` / `_CACHE_RELATIONSHIP`),
not guessed. `GROUP_AFFINITY` is 16 bytes (`u64 Mask` + `u16 Group` +
`u16 Reserved[3]`), 8-byte aligned (`KAFFINITY` is pointer-sized). All three
record types start their type-specific fields at buffer offset +8 (after the
common `Relationship`/`Size` u32 pair at +0/+4):

- `RelationProcessorCore` (0): `Flags`@+8(u8), `EfficiencyClass`@+9(u8),
  `Reserved[20]`@+10..+29, `GroupCount`@+30(u16, ends +32), `GroupMask[0].Mask`
  @+32(u64) - 32 is already 8-aligned. Matches the pre-existing
  `performance/perf_cache_mountain.cb` prototype exactly.
- `RelationNumaNode` (1): `NodeNumber`@+8(u32), `Reserved[18]`@+12..+29,
  `GroupCount`@+30(u16, ends +32), `GroupMask.Mask`@+32(u64) - 32 already
  8-aligned. Same relative layout as ProcessorCore by coincidence (both pad to
  +32 for the mask).
- `RelationCache` (2): `Level`@+8(u8), `Associativity`@+9(u8), `LineSize`@+10(u16),
  `CacheSize`@+12(u32), `Type`@+16(u32 enum), `Reserved[18]`@+20..+37,
  `GroupCount`@+38(u16, ends +40), `GroupMask.Mask`@+40(u64) - 40 already
  8-aligned. This one differs from the plan's initial guess by ending up at
  +40 (not miscounted) after working through the extra 4-byte `Type` field and
  the shorter 18-byte (not 20-byte) reserved block that `CACHE_RELATIONSHIP`
  actually has (vs `NUMA_NODE_RELATIONSHIP`'s 18 after a 4-byte NodeNumber -
  both structs pad to the same +8-relative sub-offset of 24 before GroupCount,
  it is `CACHE_RELATIONSHIP`'s extra Level/Assoc/LineSize/CacheSize/Type fields
  ahead of Reserved that push its GroupCount, and therefore its mask, 8 bytes
  further out than Core/Numa).

Confidence: HIGH. These are not judgment calls - they were read directly off
the installed SDK header, then independently confirmed by running a smoke
program that prints the walked topology and comparing it against the known
architecture of the dev box (see below). No defensive/uncertain fallback
logic was needed for the offsets themselves; the uniform-degradation fallback
in the plan is kept for the unrelated case of the Ex call failing outright
(e.g. old Windows, restrictive sandbox) or returning zero records.

**L3 dedupe**: exercised and needed. The dev box's `GetLogicalProcessorInformationEx`
walk produces exactly 2 distinct L3 records after dedupe (one per CCX) even
though L3 `RelationCache` records repeat per-subunit in the raw buffer; the
`_topo_llc_mask_seen` linear scan over `llcMask[0..llcCount-1]` collapsed the
duplicates correctly (verified by the resulting `llcCount == 2`, not a larger
number, and by the "llcMask pairwise disjoint" test assertion passing).

**EfficiencyClass on the dev box** (AMD Ryzen AI 9 365 / Strix Point, 4x Zen5
performance + 6x Zen5c compact): Windows DOES distinguish the two core types -
physical cores 0-3 report `EfficiencyClass = 1`, physical cores 4-9 report
`EfficiencyClass = 0`. `cpu_mask_perf_cores()` isolates exactly the 4 Zen5
cores (mask `0xff`, i.e. logical CPUs 0-7, the SMT sibling pairs of physical
cores 0-3) and `cpu_mask_efficiency_cores()` isolates the 6 Zen5c cores (mask
`0xfff00`, logical CPUs 8-19). This also lines up 1:1 with the LLC split
(`cpu_mask_llc(0) == 0xff`, `cpu_mask_llc(1) == 0xfff00`) - on this machine
the two Zen5/Zen5c core classes sit on two separate CCXs/LLC domains, so
`cpu_mask_perf_cores()`/`cpu_mask_efficiency_cores()` and `cpu_mask_llc(0)`/
`cpu_mask_llc(1)` happen to produce identical partitions. Full observed
snapshot: `logicalCount=20 physicalCount=10 llcCount=2 numaCount=1`; per-core
`(eff, mask)`: core0(1,0x3) core1(1,0xc) core2(1,0x30) core3(1,0xc0)
core4(0,0x300) core5(0,0xc00) core6(0,0x3000) core7(0,0xc000)
core8(0,0x30000) core9(0,0xc0000); `llcMask[0]=0xff llcMask[1]=0xfff00`;
`numaMask[0]=0xfffff`. Captured via a throwaway smoke `.cb` (not checked in)
before wiring the machine-independent assertions into
`Test/test_threadpool.cb::testCpuTopology`.

**CMake core-file deployment mechanism**: `CMakeLists.txt` (~line 255) globs
`file(GLOB_RECURSE CORE_FILES CONFIGURE_DEPENDS "${CFLAT_DIR}/core/*")` and a
custom command copies the whole `core/` directory to
`$<TARGET_FILE_DIR:cflat>/core` (i.e. `x64/<Config>/core/`) whenever any core
file's mtime is newer than a stamp file. `CONFIGURE_DEPENDS` re-globs at build
time, so `core/topology.cb` was picked up automatically with **no CMakeLists.txt
change** - confirmed by `x64/Release/core/topology.cb` existing after a plain
`cmake_build.bat release` with no other edits.

**Deviations from the plan**: none of substance. The worked example in
doc/HPC.md computes the pool worker count as `popcount(cpu_mask_llc(0))`
inline (no new helper) rather than inventing an unplanned
"mask-to-count" API, since the plan did not call for one and CLAUDE.md's
bar is against adding unplanned surface.

**Benchmark evidence (dev box)**: `performance/hpc/topology_pingpong_bench.cb`
measures cache-line ping-pong latency (two threads alternate ownership of one
shared `atomic_counter` via a busy-wait spin, no yield/pause - a direct
line-transfer-latency probe) between physical cores picked via the new
`cpu_mask_llc()` helper, in three pinned configurations: two distinct
physical cores inside `cpu_mask_llc(0)` (intra-CCX0, masks `0x1`/`0x4`), two
distinct physical cores inside `cpu_mask_llc(1)` (intra-CCX1, masks
`0x100`/`0x400`), and one core from each domain (cross-CCX, masks
`0x1`/`0x100`). Threads are started and pinned via `Thread.setAffinity`
before a shared "go" flag releases them into the timed loop (2,000,000 round
trips per run, 5 reps per configuration, minimum reported - same
fresh-process-per-variant/best-of-N discipline as `perf_cache_mountain.cb`).
Compiled with `-O2` via the worktree's `x64/Release/cflat.exe` and run 3
times on the dev box (AMD Ryzen AI 9 365 / Strix Point, 20 logical / 10
physical cores, `llcMask[0]=0xff` covering the 4 Zen5 performance cores,
`llcMask[1]=0xfff00` covering the 6 Zen5c compact cores):

| run | intra-CCX0 (ns/rt) | intra-CCX1 (ns/rt) | cross-CCX (ns/rt) | cross/intra0 | cross/intra1 |
|-----|--------------------|---------------------|---------------------|---------------|---------------|
| 1   | 94.34              | 105.28               | 191.16               | 2.03x         | 1.82x         |
| 2   | 84.29              | 102.59               | 185.79               | 2.20x         | 1.81x         |
| 3   | 90.98              | 108.20               | 196.34               | 2.16x         | 1.81x         |

Cross-CCX line transfer is consistently ~1.8-2.2x slower than either
intra-CCX case (best-of-5 minimums cluster at ~90 ns intra-CCX0, ~105 ns
intra-CCX1, ~190 ns cross-CCX across all 3 runs) - directionally consistent
with the ~2.8x prior L3-latency measurement in `perf_cache_mountain.cb`
(that number is one-way pointer-chase latency at the L3 plateau, not a
round-trip line-bounce, so the two are not expected to match exactly, only
to agree in direction and rough order of magnitude). Zen5c (intra-CCX1) is
measurably slower than Zen5 (intra-CCX0) for this line-bounce pattern too
(~10-20 ns higher), consistent with `cpu_mask_llc(0)`/`cpu_mask_llc(1)`
lining up with the EfficiencyClass split already documented above. This
confirms `cpu_mask_llc()` is picking up a real, consistently measurable
hardware boundary on this machine, which is the G5 helper's whole point.
