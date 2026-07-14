# HPC G7: Huge pages (MEM_LARGE_PAGES / MADV_HUGEPAGE)

Status: DONE (as-built) - Phases 1-3 landed 2026-07-11. Phase 4 (arena/kernel-buffer
integration) intentionally not started - gated on real benchmark evidence, per plan.

Landed: `os.huge_page_bytes()` / `os.huge_pages_enable()` / `os.vm_reserve_commit_ex`
(Windows privilege dance + retry-without-MEM_LARGE_PAGES fallback; POSIX
`_vm_reserve_commit_core(size, node, huge)` generalized with a 2MB granule +
best-effort `madvise(MADV_HUGEPAGE)` on Linux, `huge=false` byte-identical to
before); `vmem_huge_pages_available()` / `vmem_huge_page_bytes()` /
`vmem_alloc_huge()` / `vmem_alloc_numa_huge()` in `core/vmem.cb`; graceful-degrade
asserts added to `Test/test_threadpool.cb` (testNumaDomain); "Huge pages" section
in `doc/HPC.md`; `performance/hpc/hugepage_bench.cb` (dependent 2MB-strided
pointer-chase, normal vs huge). Gates green: test.bat 45/45, test_lsp.bat
204/204, example.bat 89/0/24, WSL test.sh 159/0/16.

**Deviation from plan**: `os.windows.cb`'s `LookupPrivilegeValueA` /
`OpenProcessToken` / `AdjustTokenPrivileges` link against `advapi32.lib`, which
was not in the compiler's default Windows link-library list (only
kernel32/ws2_32/ntdll were). Added `advapi32.lib` to the default link args in
`LLVMBackend.h` and to both the x64 and x86 synthetic-import-lib generators in
`LLVMBackend.cpp` (mirroring the existing kernel32/ws2_32/ntdll/dbghelp
entries exactly) - a minimal, mechanical, low-risk addition, not a
grammar/codegen change, but technically outside "pure core-library change."
Flagged here since the plan explicitly scoped this as compiler-free.

On the dev box used for verification, `SeLockMemoryPrivilege` is not granted
(expected - it requires an admin-granted account right plus re-login), so
`hugepage_bench.exe` runs as an honest, self-labelled A/A comparison
(`vmem_huge_pages_available() == false`, ~36.0 ns/access for both variants).
The graceful-degrade path (Phase 3's required test asserts) is what actually
exercises on this hardware; the huge-vs-normal win is unverified here for lack
of a privileged run, consistent with the plan's stated risk.

---

Status (original): PROPOSED
Created: 2026-07-11
Parent: internal/plan/hpc-gaps.md (G7, first bullet - the last unclaimed
item in that section besides the documented list<int[]> restriction)
Related: internal/plan/numa-domains.md (the vm_* layering this rides on)

Goal: let a program back a multi-GB kernel buffer with 2 MB pages instead
of 4 KB pages, cutting dTLB misses and page-walk cost on the streaming and
random-access kernels in core/hpc. Pure core-library change - no compiler,
grammar, or codegen work.

Non-goal: flipping the default allocators over. block_allocator /
bucket_allocator / page_pool hand out 64 KB pages; backing those with 2 MB
pages would waste memory for no TLB win. Huge pages stay an explicit
opt-in at the vmem tier for big buffers only.

---

## Why it is not just "pass one more flag"

Three constraints shape the design, and each one has bitten this layer
before:

1. **The 64 KB alignment contract.** BucketAllocator masks `ptr & ~0xFFFF`
   to find its page header, so every vmem region must be 64 KB-aligned.
   That is why the POSIX path in `os.cb` over-maps by one 64 KB granule,
   returns the first aligned address inside, and stashes `[pad, total]` in
   the 16 bytes before the user pointer (`_vm_reserve_commit_core`). A huge
   region wants 2 MB alignment, which strictly implies the 64 KB one - but
   the over-map granule and the header placement both have to change, and
   they must change in the one shared core so `vm_release` stays symmetric.
2. **Windows large pages are all-or-nothing at reserve time.**
   `MEM_LARGE_PAGES` requires `MEM_RESERVE|MEM_COMMIT` in a single call and
   the size to be a multiple of `GetLargePageMinimum()`. There is no
   reserve-now-commit-later. So `vm_reserve` / `vm_commit` stay normal-page
   only; huge pages attach to the one-shot `vm_reserve_commit` path.
3. **Privilege / configuration can always say no.** Windows needs
   `SeLockMemoryPrivilege` (an account right an admin grants, then a
   re-login); Linux needs THP enabled or hugetlb pages preallocated; macOS
   has no usable lever at all on Apple Silicon. Every one of those is a
   runtime "no" the library must absorb without failing the allocation.

## Platform matrix

| | Mechanism | Needs | If unavailable |
|---|---|---|---|
| Windows | `VirtualAlloc(..., MEM_RESERVE\|MEM_COMMIT\|MEM_LARGE_PAGES, PAGE_READWRITE)`; NUMA variant via `VirtualAllocExNuma` with the same flags | `SeLockMemoryPrivilege` enabled in the process token; size rounded up to `GetLargePageMinimum()` (2 MB on x64) | fall back to a normal `VirtualAlloc` |
| Linux | normal `mmap` + `madvise(addr, len, MADV_HUGEPAGE)` on the 2 MB-aligned user range (transparent huge pages) | `/sys/kernel/mm/transparent_hugepage/enabled` in `madvise` or `always` mode. No privilege needed | keep the normal mapping (madvise failing is not fatal) |
| macOS | none | - | plain mapping; honest no-op |

Linux design note: **transparent huge pages (`MADV_HUGEPAGE`), not
`MAP_HUGETLB`.** `MAP_HUGETLB` needs the admin to have preallocated
`vm.nr_hugepages` and hard-fails with ENOMEM otherwise, which makes it a
bad default for a library. THP is best-effort by construction and needs no
sysadmin step, which is exactly the "degrade gracefully" behaviour G7 asks
for. If a user later needs guaranteed (non-swappable, non-splittable)
hugetlb, that is a follow-on flag, not v1.

macOS design note: `VM_FLAGS_SUPERPAGE_SIZE_2MB` exists but is x86-64 only
and effectively dead on Apple Silicon. macOS is the no-op tier here, same
as it is for NUMA binding and CPU affinity - consistent with the G5/NUMA
precedent, and `huge_pages_available()` returning false is the honest
answer rather than a lie the caller cannot detect.

---

## API surface

Deliberately small. Two queries plus two allocators; the free path is
unchanged, because both the Windows `VirtualFree(MEM_RELEASE)` and the
POSIX header-stash release already work on a huge-backed region.

`core/os.cb` (namespace `os`):

```
i64  huge_page_bytes()          // 2097152 where huge pages are usable, else 0
bool huge_pages_enable()        // Windows: AdjustTokenPrivileges(SeLockMemoryPrivilege).
                                // Linux: probe THP mode. macOS: false. Memoized.
void* vm_reserve_commit_ex(i64 size, int node, bool huge)   // the ONE implementation
```

`vm_reserve_commit(size)` and `vm_reserve_commit_numa(size, id)` become
thin wrappers over `vm_reserve_commit_ex(size, id_or_-1, false)`, so the
alignment/header logic stays in a single place - the same discipline the
NUMA work applied when it introduced `_vm_reserve_commit_core`.

`core/vmem.cb`:

```
bool  vmem_huge_pages_available()      // -> os.huge_page_bytes() != 0 (after enable attempt)
i64   vmem_huge_page_bytes()
void* vmem_alloc_huge(i64 size)              // best effort; degrades to normal pages
void* vmem_alloc_numa_huge(i64 size, int n)  // huge + node-bound
// freed by the existing vmem_free / vmem_free_numa - no new free entry point
```

**Degrade-silently, query-explicitly.** `vmem_alloc_huge` never fails just
because huge pages are unavailable - it returns normal-page memory, and the
caller who cares (a benchmark labelling its run, a kernel deciding on a
blocking strategy) calls `vmem_huge_pages_available()` first. The
alternative - a `bool* gotHuge` out-param, or a process-global "was the
last alloc huge" flag - is either un-idiomatic for this codebase or
thread-unsafe, and neither buys anything a pre-flight query does not.

Size is rounded up to `huge_page_bytes()` internally. That is invisible to
callers: the Windows release takes size 0, and the POSIX header records the
true total.

---

## Phases

### Phase 1 - Windows (the one with the privilege dance)

`core/os.windows.cb` new externs + types:
- `GetLargePageMinimum() -> win_size`
- `OpenProcessToken(void* proc, u32 access, void** tok)`
- `LookupPrivilegeValueA(const char* sys, const char* name, LUID* luid)`
- `AdjustTokenPrivileges(void* tok, win_int disableAll, TOKEN_PRIVILEGES* new, u32 len, void* prev, u32* retLen)`
- structs `LUID { u32 LowPart; i32 HighPart; }`,
  `LUID_AND_ATTRIBUTES`, `TOKEN_PRIVILEGES { u32 PrivilegeCount; LUID_AND_ATTRIBUTES Privileges[1]; }`
- constants `TOKEN_ADJUST_PRIVILEGES=0x20`, `TOKEN_QUERY=0x08`,
  `SE_PRIVILEGE_ENABLED=0x02`, `MEM_LARGE_PAGES=0x20000000`.
  `GetCurrentProcess` and `CloseHandle` already exist.

`os.huge_pages_enable()`: open the process token, look up
`SeLockMemoryPrivilege`, adjust, and - critically - **re-check
`GetLastError() == ERROR_SUCCESS`**, because `AdjustTokenPrivileges`
returns TRUE even when it did not grant the privilege (the classic Win32
trap here). Memoize the result in a file-scope int (0=unknown, 1=yes,
2=no); this is called once per process, so a plain non-atomic memo behind
the existing lazy-init idiom is fine, but keep it idempotent.

`vm_reserve_commit_ex(size, node, huge=true)`: round size up to
`GetLargePageMinimum()`, OR in `MEM_LARGE_PAGES`, and on a nullptr result
retry once without it. Both the plain and the `ExNuma` call take the same
flags, so the NUMA composition is free.

### Phase 2 - Linux (alignment is the real work)

`core/os.posix.cb`: `extern win_int madvise(void* addr, win_size length, i32 advice);`
plus `MADV_HUGEPAGE = 14` (Linux; guard it out on `__MACOS__`, where the
number means something else entirely).

THP probe for `huge_page_bytes()`: read
`/sys/kernel/mm/transparent_hugepage/enabled` and accept `[madvise]` or
`[always]`. The meminfo-reading helpers `_numa_meminfo_read` /
`_numa_meminfo_value` in `os.cb` already do exactly this shape of small
sysfs read - reuse the pattern (a small `_read_sysfs_line` extracted from
them, rather than a third hand-rolled copy).

Generalise `_vm_reserve_commit_core(size, node)` to
`_vm_reserve_commit_core(size, node, huge)`:
- over-map granule becomes `huge ? 2 MB : 64 KB`, and `total = size + gran`
  as before;
- the user pointer is the first `gran`-aligned address inside the mapping,
  so a huge region is 2 MB-aligned (which satisfies the 64 KB contract);
- the 16-byte `[pad, total]` header still sits immediately before the user
  pointer - i.e. in the *previous* huge-page-sized span, which is fine
  because we only `madvise` the `[userPtr, userPtr+size)` range and THP
  works on aligned ranges within a mapping;
- `madvise(userPtr, roundedSize, MADV_HUGEPAGE)` after the mmap (and after
  the existing `_numa_mbind` when a node is bound - order matters only in
  that both must succeed on the *same* mapping; mbind failure still unmaps
  as it does today, madvise failure does not).

`vm_release` is untouched: it reads `pad`/`total` from the header exactly
as now. **This is the invariant to guard in review** - the whole reason the
NUMA work put the alignment logic in one shared core.

### Phase 3 - vmem wrappers, tests, docs

- `core/vmem.cb`: the four functions above, in the file's existing comment
  style.
- Tests: extend **`Test/test_threadpool.cb`** (its `testNumaDomains` block
  already asserts `vmem_alloc_numa` is non-null / 64 KB-aligned / writable
  at both ends / symmetrically freed - the huge asserts belong right
  beside it, and that is where the vmem-tier coverage already lives; no new
  test file). Asserts that must hold on **every** platform and in CI where
  the privilege is certainly *not* granted:
  - `vmem_alloc_huge(4 MB)` is non-null, 64 KB-aligned, writable at
    `[0]` and `[len-1]`, and `vmem_free`s cleanly - whether or not it got
    huge pages (this *is* the graceful-degrade test);
  - if `vmem_huge_pages_available()` is true, additionally assert 2 MB
    alignment;
  - `vmem_alloc_numa_huge` on `topo.domainId(0)` behaves the same;
  - `vmem_huge_page_bytes()` is 0 or 2097152, never anything else.
- `doc/HPC.md`: a "Huge pages" section - what it buys (TLB), the API, and
  the admin steps per platform (Windows: `secpol.msc` -> Local Policies ->
  User Rights -> "Lock pages in memory" -> add the account -> log out and
  back in; Linux: THP mode, and that `always` mode gets you huge pages with
  no code change at all). State plainly that macOS has no huge-page lever.
- Update `internal/plan/hpc-gaps.md` G7 with a DONE line, per the file's
  convention.

### Phase 4 (optional, only if Phase 3's numbers justify it)

An opt-in huge-backed segment source for `arena_allocator`'s large-alloc
path and for `Mat`/kernel buffers in core/hpc. Do not start this before the
benchmark exists - the whole point is to spend it where the TLB actually
hurts.

---

## Verification

Benchmark: **`performance/hpc/hugepage_bench.cb`** (new file under
`performance/`, which is not swept by `test.bat`'s `Test/*.*` wildcard -
the no-new-test-files rule applies to `Test/`, and G2/G5 both landed their
benchmarks this way).

It must be *TLB-bound*, not bandwidth-bound, or it will measure nothing: a
dependent pointer-chase over a 2-4 GB buffer with a random permutation of
2 MB-strided targets, so each access misses the dTLB and the huge/normal
difference shows up as page-walk cost. Report ns/access for normal vs huge
over the same buffer size, and print `vmem_huge_pages_available()` so a run
without the privilege is self-labelling rather than a silently
uninteresting A/A comparison. Expect a meaningful win on the dev box only
when the buffer's page count blows past the ~1.5-2K dTLB entries.

Gates (per hpc-gaps.md "Constraints"): build Release, `test.bat` green,
`example.bat` green. `test_lsp.bat` is **not** needed - no
MainListener/LspServer change. Re-verify `test.sh` on WSL, because Phase 2
edits the shared POSIX `_vm_reserve_commit_core` that *every* allocator on
Linux and macOS depends on - this is the highest-blast-radius part of the
change, and a regression there breaks everything, not just huge-page users.

## Risks

- **Regressing the normal path.** Phase 2 touches the function behind every
  POSIX `vmem_alloc`. Mitigation: `huge=false` must produce byte-identical
  behaviour to today (same 64 KB granule, same header), and the Linux/macOS
  suites are the proof.
- **The Windows privilege is not grantable in many environments** (CI,
  locked-down corp images, containers). Hence: never fail an allocation
  over it, and make the test assert the degrade path.
- **Large-page allocation can fail on a fragmented system even with the
  privilege held** - the kernel needs physically contiguous 2 MB blocks.
  The retry-without-MEM_LARGE_PAGES fallback covers this; it is not a bug
  report when it happens after hours of uptime.
- **Silent no-op on Linux** if THP is set to `never`. The probe reports it,
  and `doc/HPC.md` says how to check.

## Cost

Roughly: Phase 1 half a day (the token dance is fiddly but well-trodden),
Phase 2 half a day (the alignment change is small, its blast radius is
not), Phase 3 a half day including the benchmark. Sonnet tier is the right
one for all three - the plan is specific and the files are named - with the
Phase 2 diff reviewed carefully against the alignment invariant.
