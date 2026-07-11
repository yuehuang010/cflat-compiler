# NUMA Domains: query, acquire/isolate, pinned threads, local memory

Status: DONE (all 4 steps landed 2026-07-11)
Created: 2026-07-11

Validation constraint (user, 2026-07-11): the dev box is single-NUMA-node,
so PERFORMANCE cannot be validated - the bar is that every API call WORKS
(returns correct data / succeeds / fails cleanly) on Windows AND on WSL
(test.sh gate applies).
Parent: internal/plan/hpc-gaps.md (G5 follow-on; "per-node arenas if NUMA
hardware ever matters" is this plan)

Scope decision (user, 2026-07-11): SINGLE PROCESS ONLY. The library
manages the calling process's own threads and memory placement; evicting
or isolating OTHER processes (cgroup cpuset partitions, cgroup.kill,
system-wide CPU sets) is OUT OF SCOPE entirely. This removes the
root-only Linux cgroup tier from an earlier draft and makes "kill the
process subtree" simply "kill the thread".

Goal: extend core/topology.cb (query) and add core/numa.cb (acquisition
class) so an HPC program on a 1-8 NUMA-domain compute node can:

1. Query the number of available NUMA domains and their OS domain IDs.
2. Query a specific domain ID for core count and domain-local memory.
3. Acquire a domain by ID: reserve it in-process (registry prevents
   double-acquire) and confine the process's OTHER threads away from it
   where the OS supports process affinity.
4. Release the domain, optionally after killing any threads still
   running in it.
5. Acquire a core-pinned/bound thread from an acquired domain, with its
   memory allocation steered to that domain.
6. Release a pinned thread back to the domain (killing it if still
   running).

Addendum honored: on a single-domain node, 5/6 still work - the domain
object degrades to "the whole machine" and pinned threads still get
per-core affinity protection where the OS supports affinity.

Platforms: Windows + Linux are the real implementations; macOS is near
no-op (1 synthetic domain, no affinity - everything succeeds at the
weakest tier so portable code still runs). All OS calls go through
`namespace os` in os.cb per the repo rule; raw externs live in
os.windows.cb / os.posix.cb.

---

## Ground truth per OS (what is actually possible, single-process scope)

This table drives the API.

| Capability | Windows | Linux | macOS |
|---|---|---|---|
| Enumerate node IDs | GetLogicalProcessorInformationEx RelationNumaNode (NodeNumber@+8 - currently DISCARDED by _topo_walk) | /sys/devices/system/node/online (node id currently DISCARDED) | none: 1 synthetic domain, id 0 |
| Node free/total memory | GetNumaAvailableMemoryNodeEx(node) free; total per node NOT exposed by a documented user-mode API -> report -1 for total, free is real | /sys/devices/system/node/nodeN/meminfo (MemTotal + MemFree, kB lines) | hw.memsize total; free = -1 (host_statistics64 not worth binding for the no-op tier) |
| Node-local allocation | VirtualAllocExNuma(GetCurrentProcess(), ..., node) | mmap + mbind(MPOL_BIND, nodemask) (raw syscall, no libnuma dep) | plain vm_reserve_commit (no-op placement) |
| Pin thread to core | SetThreadAffinityMask (exists) | pthread_setaffinity_np (exists) | none; QoS only (exists) |
| Bind thread's future page allocs to node | pinning + first-touch (Windows honors first-touch on the running node) | set_mempolicy(MPOL_BIND) called ON the thread (self-only) | none |
| Confine the process's un-pinned threads off a domain | SetProcessAffinityMask (+ CPU Sets as intent) | sched_setaffinity(0, ...) - NOTE: per-THREAD on Linux, see PROCESS-tier caveat | none |
| Kill a handed-out thread on release | TerminateThread (os.thread_kill exists) | pthread_cancel+detach (os.thread_kill exists) | same |

What is NOT here anymore (single-process scope): evicting other
processes, cgroup cpuset partitions, cgroup.kill, migrate_pages. If
multi-process isolation is ever needed it is its own plan.

Acquisition still reports (and can demand) a confinement tier, because
the OSes differ in what they can do for our own process:

```cflat
// Confinement levels, weakest to strongest.
const int NUMA_ISOLATE_NONE    = 0;  // bookkeeping only (macOS; or caller opt-out)
const int NUMA_ISOLATE_PROCESS = 1;  // this process's un-pinned threads are kept
                                     // OFF acquired domains (affinity complement);
                                     // handed-out threads are pinned INSIDE
```

`acquire(id, required)` fails cleanly only on: unknown id, id already
acquired in-process, or required==PROCESS on macOS (no affinity API).
The original "OS scheduler is bound to domain X" failure mode belonged
to system-wide isolation and is gone with it; within one process every
valid domain is acquirable.

PROCESS-tier caveat (Linux): sched_setaffinity(0) affects only the
CALLING thread, not the whole process (Windows SetProcessAffinityMask
does affect all threads). Confinement of "the rest of the process" on
Linux therefore means: main thread constrained at acquire; threads
CREATED AFTER acquire inherit the constraint; pre-existing user threads
are not chased (enumerating /proc/self/task and re-pinning foreign
threads is intrusive - documented limitation). In practice HPC programs
acquire domains at startup before spawning anything, which this covers.

---

## Part A - topology.cb query extensions (features 1-2)

Pure additive changes to the existing snapshot; no behavior change for
current callers.

### Snapshot fields

```cflat
struct CpuTopology {
    ... existing ...
    i32[8] numaId = default;   // OS node ID per numaMask slot (sparse-safe)
};
```

- Windows `_topo_walk`: RelationNumaNode already read; also capture
  `NodeNumber` at +8 (u32) into `numaId[numaCount]`.
- Linux `_topo_build`: both node loops already know `node`; store it.
- macOS + `_topo_build_uniform`: `numaId[0] = 0`.

### New query API (topology.cb, alongside cpu_mask_numa)

```cflat
// Feature 1: domain count is the existing cpu_numa_count(); IDs:
int numa_domain_id(int index);          // OS node ID of snapshot slot index; -1 OOR
int numa_domain_index(int id);          // inverse lookup; -1 if unknown id

// Feature 2: per-domain detail (id-keyed, since users hold IDs):
struct NumaDomainInfo {
    int id             = -1;
    int physicalCores  = 0;   // cores whose coreMask intersects the node mask
    int logicalCpus    = 0;   // popcount(numaMask)
    u64 cpuMask        = 0;
    i64 memTotalBytes  = -1;  // -1 = OS does not expose it (Windows, macOS free-only cases)
    i64 memFreeBytes   = -1;  // -1 = unknown
};
bool numa_domain_info(int id, NumaDomainInfo* out);
```

Core counts come from the memoized snapshot. Memory is queried LIVE on
each call (free memory changes; do not memoize), via new os.cb surface:

```cflat
namespace os {
    // Bytes of free memory local to NUMA node `id`; -1 if unknown.
    i64 numa_free_bytes(int id);
    // Bytes of total memory local to node `id`; -1 if the OS hides it.
    i64 numa_total_bytes(int id);
}
```

- Windows externs (os.windows.cb): `GetNumaAvailableMemoryNodeEx(u8 node,
  u64* bytes)` (kernel32). Total: not exposed per-node by a documented
  user-mode API -> return -1 and document.
- Linux (os.posix.cb or file-parse in topology-side helper): parse
  `/sys/devices/system/node/nodeN/meminfo` lines `Node N MemTotal:`/
  `Node N MemFree:` (kB). Reuse `_sysfs_read`-style parsing; since os.cb
  must own the dispatch, put a thin `os.numa_*_bytes` that on POSIX calls
  a helper compiled in os.posix.cb (fopen/fread already extern there).
- macOS: total = `hw.memsize` sysctl for id 0; free = -1 (keep it
  minimal; host_statistics64 binding is not worth it for a no-op tier).

### 64-CPU / processor-group limit (unchanged, documented)

Masks stay u64 group-0, same as all of topology.cb. On a multi-group
Windows box (>64 LPs), nodes whose CPUs live outside group 0 will show
cpuMask == 0; `numa_domain_info` still reports id + memory. Document
next to the existing 64-LP limit note. (Lifting this is its own plan.)

---

## Part B - core/numa.cb acquisition class (features 3-6)

New core file `core/numa.cb` (explicit import), imports topology.cb,
thread.cb, vmem.cb, mutex.cb. This is the "guide the user" layer: the
class shape makes the good pattern (one domain object -> pinned threads +
local memory from THAT domain) the path of least resistance.

### The class

```cflat
// One acquired NUMA domain. Not copyable (owning). Acquire via
// NumaDomain.acquire(); release() or scope-exit returns it to the OS.
struct NumaDomain {
    int  id         = -1;
    int  level      = NUMA_ISOLATE_NONE;   // level actually achieved
    u64  cpuMask    = 0;                   // logical CPUs of the domain
    // internal: per-physical-core hand-out state
    u64  _freeCores  = 0;                  // one bit per AVAILABLE physical core (lowest sibling)
    NumaThread*[64] _threads = default;    // live hand-outs
    mutex _lock = default;

    // Feature 3. Returns nullptr if `id` is unknown, already acquired
    // in-process, or `required` cannot be achieved (PROCESS on macOS).
    static NumaDomain* acquire(int id, int required);

    // Feature 4. killRemaining: kill still-live NumaThreads before
    // dissolving the confinement. Idempotent.
    void release(bool killRemaining);
    ~NumaDomain();   // release(false)

    // Feature 5. Hand out a thread pinned to one free physical core of
    // this domain (lowest-sibling logical CPU), with its memory policy
    // bound to this node where the OS supports it. nullptr when no free
    // core remains (caller sizes work to physicalCores).
    NumaThread* acquireThread(function<void(void*)> entry, void* arg);

    // Feature 6. If the thread is still running: kill it (thread_kill /
    // pthread_cancel semantics - document the same caveats os.cb does).
    // Returns the core to _freeCores.
    void releaseThread(NumaThread* t);

    // Domain-local memory (the memory half of the guidance story):
    void* allocLocal(i64 size);            // node-bound pages (see os.vm_* below)
    void  freeLocal(void* p);
    NumaDomainInfo info();                 // live re-query of feature 2
};

// A core-pinned thread handed out by a domain. join() for graceful use;
// releaseThread(kill) for the hard path.
struct NumaThread {
    int  core     = -1;   // physical-core index within the domain
    u64  pinMask  = 0;    // single logical CPU bit
    // internal: os thread handle, done-flag, owner backref
    bool join(int timeoutMs);   // graceful completion
};
```

Decisions settled here:

- **One thread per PHYSICAL core** is the hand-out unit (lowest SMT
  sibling), because that is the HPC-correct default; SMT siblings are
  never handed out as separate "cores". A domain with N physical cores
  supports N concurrent NumaThreads. (If a user wants SMT pairs they can
  still build masks by hand with the existing topology helpers.)
- **Memory binding is thread-side + allocation-side**, not magic:
  `acquireThread` sets MPOL_BIND on Linux inside the thread trampoline
  (set_mempolicy is self-only), so the thread's malloc/stack pages are
  node-local by first-touch; `allocLocal` gives explicit node-bound
  buffers on Windows+Linux. On Windows there is no per-thread mempolicy;
  pinning + first-touch is the mechanism and allocLocal
  (VirtualAllocExNuma) is the explicit lever. Document this asymmetry.
- **Double-acquire protection**: a module-level registry (mutex + u64
  acquired-id bitmask) makes acquire(id) fail if already held in-process.
- **Kill semantics**: releaseThread(t) / release(true) kill still-running
  threads with os.thread_kill (documented unsafe-skip-cleanup semantics,
  same as thread.cb - TerminateThread on Windows, pthread_cancel+detach
  on Linux with the packet-leak caveat os.cb already documents). Prefer
  t->join() for graceful shutdown; kill is the hard path.

### Confinement tiers - what acquire(id, level) actually does

**NUMA_ISOLATE_NONE** (always succeeds on a valid, un-acquired id):
bookkeeping only. Domain object + thread hand-out + allocLocal; no
scheduler interference. This is the macOS tier (threads get QoS steering
via the existing cpu_qos_for_mask instead of pinning; allocLocal is
plain vmem_alloc).

**NUMA_ISOLATE_PROCESS** (Windows + Linux; the default `required`):
everything in NONE, plus keep the process's un-pinned threads OFF the
acquired domain:
- Set the process affinity to the complement of all acquired domains'
  masks: Windows `SetProcessAffinityMask` (new extern), Linux
  `sched_setaffinity(0, ...)` (calling thread + inherited by threads
  spawned after - see the PROCESS-tier caveat above).
  - Edge: if the complement would be empty (single-domain machine -
    the addendum case), SKIP the process-affinity step, still succeed,
    and hand out pinned threads; record level = PROCESS. Pinning is the
    protection that remains, exactly as the addendum asks.
  - NumaThreads themselves are exempt by construction: per-thread
    affinity (SetThreadAffinityMask / pthread_setaffinity_np on the
    handed-out thread) overrides the process-level complement.

    RESOLVED (2026-07-11, step 3 - empirical probe on the dev box, a
    single-domain 20-LP Ryzen; probe was a throwaway C program, not
    checked in). Findings:
      1. The Windows subset constraint is REAL. After
         SetProcessAffinityMask(complement-of-cpu0), a subsequent
         SetThreadAffinityMask(thread, cpu0) FAILS (returns 0,
         GetLastError == 87 ERROR_INVALID_PARAMETER) and the thread runs
         on some other CPU. So shrinking the PROCESS affinity mask would
         break pinning a NumaThread INSIDE an acquired domain. Confirmed.
      2. CPU Sets are the fix. GetSystemCpuSetInformation returns 32-byte
         self-sized records (Size@+0, Id@+8 u32, Group@+12 u16,
         LogicalProcessorIndex@+14 u8). SetProcessDefaultCpuSets(process,
         complement-ids) succeeds AND leaves the process affinity FULL,
         so SetThreadAffinityMask(thread, cpu0) then succeeds and the
         pinned thread runs on cpu0 - i.e. an explicit per-thread pin
         BEATS the process default CPU set. Exactly the PROCESS-tier
         intent: default (un-pinned) threads are steered onto the
         complement, pinned NumaThreads keep their acquired-domain cores.

    MECHANISM CHOSEN: CPU Sets for process-level confinement, per-thread
    SetThreadAffinityMask for pinning (process affinity is NEVER touched
    on Windows). Neutral surface added to os.cb:
      - os.process_confine_cpus(u64 keepMask): Windows walks
        GetSystemCpuSetInformation, collects group-0 CpuSetIds whose
        LogicalProcessorIndex is in keepMask, calls
        SetProcessDefaultCpuSets. Linux: sched_setaffinity(0, keepMask)
        (main thread + inherited by threads spawned after; per-thread
        pins override - no subset problem, affinity is per-thread on
        Linux). macOS: false.
      - os.process_restore_cpus(u64 savedMask): Windows clears the
        default set (SetProcessDefaultCpuSets(NULL,0), savedMask ignored
        since affinity was untouched); Linux restores savedMask via
        sched_setaffinity(0,...); macOS no-op.
    SetProcessAffinityMask is NOT called for confinement anywhere.
    SetThreadSelectedCpuSets was judged unnecessary (SetThreadAffinityMask
    alone pins correctly with full process affinity, probe-proven) and is
    NOT bound - only the two used externs (GetSystemCpuSetInformation,
    SetProcessDefaultCpuSets) were added, per the repo "no loose ends"
    rule. numa.cb owns the saved-affinity registry state
    (_g_numaSavedCpus) so os.cb stays stateless.
- Windows extra (cheap, worth it): CPU Sets to declare intent -
  `GetSystemCpuSetInformation` to map CPUs->CpuSetIds,
  `SetProcessDefaultCpuSets` to the complement; pinned NumaThreads get
  `SetThreadSelectedCpuSets`. Degrade silently to affinity-only if the
  APIs fail.
- On release: restore the saved process affinity/default CPU sets
  (recompute from the registry so releasing one domain keeps the others'
  confinement intact).

### os.cb additions (all dispatch lives here, per repo rule)

```cflat
namespace os {
    i64  numa_free_bytes(int id);            // Part A
    i64  numa_total_bytes(int id);           // Part A
    void* vm_reserve_commit_numa(i64 size, int id);  // node-bound alloc
    void  vm_release_numa(void* p);          // symmetric free (POSIX hdr-page stash reuse)
    bool process_set_affinity(u64 mask);     // self; Windows SetProcessAffinityMask / Linux sched_setaffinity(0,..)
    u64  process_get_affinity();             // to save/restore on release
    void thread_bind_memory_self(int id);    // Linux set_mempolicy(MPOL_BIND); no-op elsewhere
}
```

New externs:
- os.windows.cb: `GetNumaHighestNodeNumber`, `GetNumaAvailableMemoryNodeEx`,
  `VirtualAllocExNuma`, `GetCurrentProcess`, `SetProcessAffinityMask`,
  `GetProcessAffinityMask`, and (CPU Sets, load dynamically? no -
  kernel32 static import is fine on Win10+, this repo already assumes
  modern SDK) `GetSystemCpuSetInformation`, `SetProcessDefaultCpuSets`,
  `SetThreadSelectedCpuSets`.
- os.posix.cb (Linux branch): `sched_setaffinity`, `sched_getaffinity`,
  raw syscalls `set_mempolicy` and `mbind` via `syscall(long, ...)`
  (glibc exposes syscall(); numbers x86_64: set_mempolicy=238, mbind=237
  - gate `if const` on the Linux x64 path we already target; NO libnuma
  dependency).
- macOS: nothing new.

vmem.cb: add `vmem_alloc_numa(i64 size, int node)` + free symmetry,
delegating to the os.vm_* pair, mirroring the existing vmem_alloc shape
(including the POSIX 64K-alignment header trick already in os.posix
vm code - REUSE it, do not fork the logic; mbind is applied after mmap
inside os.posix's helper so alignment handling stays in one place).

### Guidance layer (the "help the user optimize" ask)

`NumaDomainInfo` deliberately packages exactly the two sizing inputs
(physicalCores for thread count, memFreeBytes for partition size) so
"query -> size -> acquire threads/memory from the same domain object"
is the obvious flow; cross-domain mistakes require going around the
class. ThreadPool integration is OUT OF SCOPE for v1 (pool pinMask
already accepts cpu_mask_numa(i); a per-domain pool ctor can come later).

doc/HPC.md gains a "NUMA domains" section built around this worked
example - loading a very large file from disk into memory and then
processing it, with both the pages and the compute threads distributed
across domains:

```cflat
import "numa.cb";
import "filesystem.cb";

// Load `path` (tens of GB) striped across every NUMA domain, then run
// one compute worker per physical core, each touching ONLY the slice
// that lives in its own domain. Without this, a single-threaded read
// first-touches every page onto the loading thread's node and every
// core on the other nodes pays remote-access latency forever after.
struct FileShard {
    NumaDomain* domain = nullptr;   // owns the pages + the cores
    i8*  base   = nullptr;          // domain-local buffer (allocLocal)
    i64  offset = 0;                // byte range of the file in this shard
    i64  len    = 0;
};

int shardCount = cpu_numa_count();          // feature 1
FileShard[8] shards = default;
i64 fileSize = file_size(path);

// 1) Query each domain (feature 2) and acquire it (feature 3). Sizing
//    is proportional to each domain's free local memory, so an
//    asymmetric machine (or a half-full node) does not overcommit one
//    node while another sits empty. Single-domain machine: loop runs
//    once, everything below still works (addendum path).
i64 freeTotal = 0;
int i = 0;
while (i < shardCount) {
    NumaDomainInfo inf = default;
    numa_domain_info(numa_domain_id(i), &inf);
    freeTotal = freeTotal + inf.memFreeBytes;   // -1 handling elided
    i = i + 1;
}
i64 covered = 0;
i = 0;
while (i < shardCount) {
    shards[i].domain = NumaDomain.acquire(numa_domain_id(i), NUMA_ISOLATE_PROCESS);
    NumaDomainInfo inf = shards[i].domain->info();
    i64 slice = (i == shardCount - 1) ? (fileSize - covered)
              : fileSize * inf.memFreeBytes / freeTotal;
    shards[i].offset = covered;
    shards[i].len    = slice;
    // 2) The pages are BORN on the right node: allocLocal binds them
    //    (VirtualAllocExNuma / mbind) - no first-touch discipline needed
    //    for the buffer itself.
    shards[i].base   = (i8*)shards[i].domain->allocLocal(slice);
    covered = covered + slice;
    i = i + 1;
}

// 3) Parallel load (feature 5): each domain contributes ONE pinned I/O
//    thread that preads its own [offset, offset+len) range straight into
//    its domain-local buffer. The disk stripes across domains, and each
//    write lands on node-local memory - the interconnect carries nothing.
//    (One reader per domain is the right default; NVMe won't feed more.)
i = 0;
while (i < shardCount) {
    shards[i].domain->acquireThread(loadShard, &shards[i]);
    i = i + 1;
}
// join all loaders via NumaThread.join ... (elided)

// 4) Compute (features 5/6): every physical core of every domain gets a
//    worker; each worker's slice comes from ITS domain's shard, so all
//    hot reads are node-local. acquireThread also binds the thread's own
//    allocations (stack growth, scratch mallocs) to the node on Linux.
i = 0;
while (i < shardCount) {
    NumaDomainInfo inf = shards[i].domain->info();
    int w = 0;
    while (w < inf.physicalCores) {
        // worker ctx: shard i, sub-range w of inf.physicalCores
        shards[i].domain->acquireThread(computeWorker, makeCtx(&shards[i], w));
        w = w + 1;
    }
    i = i + 1;
}
// join workers, then release (features 4/6):
i = 0;
while (i < shardCount) {
    shards[i].domain->freeLocal(shards[i].base);
    shards[i].domain->release(false);   // graceful; release(true) would kill stragglers
    i = i + 1;
}
```

What the library did for the user, step by step: domain discovery
(count + ids), free-memory-proportional shard sizing (info), buffers
placed on the right node at allocation time (allocLocal), loader and
compute threads pinned to cores of the SAME node as their data
(acquireThread), un-pinned threads of the rest of the process kept off
the acquired domains (PROCESS tier), and a single release path that
restores the process affinity and can reap stragglers. On a 1-domain
box the identical code degrades to "pinned threads + one big buffer".
This example (or a trimmed version) should also land as a runnable
example/ program in a later step if example-worthy hardware exists;
the doc version is the deliverable.

---

## What this deliberately does NOT do

- Nothing multi-process: no evicting other processes, no cgroups, no
  system-wide CPU sets (user scope decision 2026-07-11). The OS
  scheduler may still run OTHER processes' threads on an acquired
  domain's cores; per-thread pinning plus the process-affinity
  complement is the whole confinement story.
- No page migration in v1 (pages are PLACED right via allocLocal/
  first-touch, never moved after the fact). POST-V1 TODO (user,
  2026-07-11): a move helper. Linux can migrate in place without libnuma:
  mbind(MPOL_MF_MOVE) over an owned range (unprivileged for private
  pages); move_pages(2) with a NULL nodes array is the placement-query
  probe for tests. Windows has NO migration API - the portable shape is
  `void* moveToDomain(p, size, d)` (Linux returns p, Windows allocs on
  the target node + copies + frees, pointer changes). Do after v1 lands.
- No libnuma dependency (raw syscalls + sysfs only).
- No >64-LP / multi-group support (existing topology.cb limit).
- cpu_mask_numa / existing topology API: unchanged, byte-compatible.

## Implementation steps

1. **Part A** (sonnet): numaId capture in all three _topo_build paths +
   uniform fallback; numa_domain_id/index/info; os.numa_free/total_bytes
   + externs; extend testCpuTopology in Test/test_threadpool.cb
   (machine-independent: ids unique, index(id(i))==i, info(id) mask ==
   cpu_mask_numa(i), memFree <= memTotal when both >= 0, unknown id ->
   false). Docs: doc/HPC.md query subsection.
2. **os layer for Part B** (sonnet): vm_reserve_commit_numa (+vmem.cb
   wrappers), process_(get|set)_affinity, thread_bind_memory_self, all
   externs. Windows path verifiable on the dev box (VirtualAllocExNuma
   on node 0 of a 1-node box still works).
3. **core/numa.cb** (opus - ownership + thread trampoline interplay +
   the Windows thread-affinity-subset-of-process-affinity ordering
   question flagged in the PROCESS-tier section): NumaDomain/NumaThread,
   registry, hand-out bookkeeping, acquireThread pin + bind,
   release/kill paths, allocLocal. Tests in Test/test_threadpool.cb
   (single-node dev box IS the addendum case - perfect for testing it:
   acquire(0, PROCESS) succeeds without the affinity-complement step,
   threads run pinned, allocLocal works, double-acquire fails, release
   restores process affinity).
   DONE 2026-07-11. core/numa.cb landed (NumaDomain/NumaThread, module
   registry, list<NumaThread*> ownership, trampoline that self-binds
   memory + steers QoS, CPU-Sets confinement resolved empirically - see
   PROCESS-tier section above). os.cb gained process_confine_cpus /
   process_restore_cpus; os.windows.cb gained GetSystemCpuSetInformation
   + SetProcessDefaultCpuSets. Tests: testNumaDomain() in
   Test/test_threadpool.cb (23/23). Deviations from the sketch: (a)
   acquireThread returns `alias NumaThread*` (borrow) and the domain owns
   the threads in a `list<NumaThread*>` instead of a raw `NumaThread*[64]`
   - the raw-pointer-array store has ambiguous move semantics in cflat;
   list<T*> has defined move-on-add + take() ownership, so it is the safe
   choice. (b) NUMA_ISOLATE_* are `const int` (compiles fine; used
   elsewhere in core). Gates: test.bat Release all pass; test.sh Release
   159/0/16 (test_threadpool 23/23 on Linux - set_mempolicy trampoline +
   pin/join/kill/mbind-allocLocal exercised); --platform macos
   cross-compile exit 0; --asan clean.
4. **Docs + example** (sonnet): doc/HPC.md "NUMA domains" section with
   the large-file worked example above + the per-OS capability table;
   note in doc/THREADING.md. No new Test/ files anywhere (extend
   existing).
   DONE 2026-07-11. doc/HPC.md gained a "### NUMA domains (`core/numa.cb`)"
   subsection (nested under "Across-core data parallelism", right after
   the existing "Pinning workers to cores" topology-masks material and
   before "Per-worker floating-point environment") with: the remote-memory/
   first-touch motivation, the query API (numa_domain_id/index,
   NumaDomainInfo/numa_domain_info with the verified -1 semantics per OS),
   the NumaDomain/NumaThread class (acquire/release, the two confinement
   tiers and exact failure modes, acquireThread/releaseThread/join,
   allocLocal/freeLocal, info()), a trimmed per-OS capability table, the
   large-file worked example (adapted from the sketch above and verified
   against the shipped core/numa.cb + core/topology.cb + core/filesystem.cb
   APIs), and an honest limitations paragraph (single-process scope,
   best-effort confinement, 64-CPU limit, no page migration in v1 with the
   move-helper TODO still visible, and that multi-node performance itself
   is unverified on the single-node dev hardware). doc/THREADING.md gained
   a short "NUMA-aware domains" subsection at the end of Thread Pool,
   pointing at HPC.md. Both files' Tables of Contents updated.

   Deviations found while verifying the worked example against the
   shipped library (fixed in the doc, not the library):
   - `join(timeoutMs)` has no portable "wait forever" sentinel: on
     Windows a negative timeout maps to `WaitForSingleObject`'s
     `INFINITE`, but the macOS/Linux polling `thread_timed_join` treats
     any negative value as an immediate timeout (`waited(0) >= ms` is
     true for `ms < 0`). The doc example joins via a bounded retry loop
     (`while (!t->join(1000)) { }`) instead of `join(-1)`; the "join
     semantics" bullet calls this out explicitly.
   - `core/filesystem.cb`'s `File.seek`/`.size()`/`.readBytes` are
     `int`-sized (no `i64` overload), so the "tens of GB" load in the
     original sketch cannot be one `readBytes` call. The doc's
     `loadShard` reads each domain's shard in bounded 64 MB chunks
     inside an `i64`-indexed loop instead.
   - `function<void(void*)>` entries are non-capturing (matching
     `Thread`/`ThreadPool`'s task shape), so the file path every loader
     thread needs is carried on a plain global (`g_loadPath`) rather
     than captured - the sketch didn't need this because it left the
     load/compute bodies unwritten.
   - `NumaDomain.acquire`/`NumaThread` match the sketch otherwise:
     `acquireThread` returns `alias NumaThread*` (a borrow - the domain
     owns it), confirmed against `Test/test_threadpool.cb`'s
     `testNumaDomain()`.

   Verification: the worked example was extracted verbatim to a
   scratch .cb file and checked with
   `x64/Release/cflat.exe <file> --check` - PASS on the first attempt
   after the two fixes above (join retry loop, chunked file read) were
   applied during drafting. Docs-only change: no build/test.bat run
   needed (nothing under cflat/, core/, or Test/ touched).

Gates per step: build Release, test.bat green; test.sh on WSL for steps
1-3 (POSIX code paths are load-bearing here - the standing skip does
NOT apply); example.bat if examples touched; no test_lsp needed (no
compiler change anywhere in this plan - it is pure library).

## Post-landing fix: TerminateThread-vs-loader-lock hang (2026-07-11)

After all 4 steps landed, one full-parallel `test.bat` run hung on
test_threadpool for the 600s suite timeout (passed standalone and on
rerun). Reproduced 1/60 running test_threadpool.exe under 16 CPU
burners; cdb stacks of the hung process were definitive: the test had
printed "23/23 passed" and the MAIN thread was stuck at process exit in
`ucrtbase!common_exit -> LoadLibraryExW -> ntdll!LdrpDrainWorkQueue`.
Root cause: `release(true)`'s TerminateThread (os.thread_kill) landed
while the just-created worker was still inside NT loader thread-init
(startup delayed by machine load); the killed thread orphaned loader
state, deadlocking ucrt's lazy exit-time LoadLibrary.

There are TWO unsafe kill windows, both loader-lock regions of a thread
that user code never sees:
1. Thread INIT: kill lands before the thread entry runs (thread start
   delayed by load) - inside LdrInitializeThunk/DLL_THREAD_ATTACH.
2. Thread EXIT: kill lands after user code returned (main delayed past
   the worker's short sleep) - inside ExitThread/DLL_THREAD_DETACH.
A first fix that only gated window 1 (`_started` flag set at shim entry,
spin before TerminateThread) still hung 1/100 under load - the cdb
stacks were identical, which is what exposed window 2.

Fix at the root in os.cb (protects thread.cb kill and the program
construct too, not just numa.cb), Windows branch of `os.thread_kill`:
- Gate 1: `__OsThreadPacket._started` is set at shim entry (past loader
  init); thread_kill spins on `_started || _done` first.
- Gate 2: SuspendThread, then re-check `_done`. If `_done` is set the
  thread finished user code and may be inside thread-exit - resume and
  WaitForSingleObject for natural exit instead of terminating (there is
  nothing left to kill). If `_done` is clear the thread is suspended
  strictly inside shim/user code - TerminateThread is safe from loader
  orphaning there. New externs: SuspendThread/ResumeThread.
POSIX behavior unchanged (pthread_cancel is deferred; the extra
field/store is inert). A kill landing inside USER code that itself
holds a lock (heap, loader via LoadLibrary, ...) remains
documented-unsafe, as before - same contract as thread.cb.

Implementation gotcha that itself caused a hang (fixed): os.cb's
thread packets are MALLOC'd, not `new`'d, so struct field defaults do
NOT apply - thread_create must explicitly zero every flag. The new
`_started` field was initially left uninitialized; a recycled heap
allocation carried a stale `_started = 1`, gate 1 passed instantly, and
the kill landed mid-loader-init again (a tight create/kill loop hung
50% of runs; cdb showed the NEXT thread stuck in LdrpInitializeThread
-> LdrpDrainWorkQueue with main spinning in gate 1). One line:
`p->_started = 0;` in thread_create.

testNumaDomain's immediate release(true)-after-acquireThread is exactly
the race trigger and stays as the regression test. Stress evidence (all
under 16 CPU-burner load): pre-fix 1 hang/60 runs of
test_threadpool.exe; gate-1-only 1 hang/100; final fix 0 hangs/0 fails
across 40 runs x 500 swept-timing kills (20,000 kills sweeping the
startup/in-sleep/exit windows) AND 100 full test_threadpool runs.

## Verification highlights

- Dev box (Windows, 1 NUMA node, 20 LP): Part A info() reports id 0,
  10 physical cores, free bytes > 0; addendum path end-to-end.
- WSL: PROCESS tier + syscall mempolicy + mbind path (single node in a
  VM: mbind to node 0 must succeed; unknown node must fail cleanly).
- macOS (when a box is available): compiles, 1 domain, NONE tier, QoS
  steering on acquireThread - cross-target codegen check from Windows
  in the interim (--platform macos), same as the topology.cb precedent.
- True multi-node behavior (real placement wins) cannot be measured on
  any current box; the design keeps all multi-node logic data-driven
  off the snapshot so a future 2-socket box exercises it without code
  changes. Record this honestly in the doc.
