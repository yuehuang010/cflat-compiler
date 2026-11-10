# Cold C++ interop compile peaks at 23-27 GB of memory

Summary: one cold compile of Test/test_cpp_interop.cb (empty cache, `--check` or `-o`) holds 23 GB
on the Mac and 27 GB on Windows. No result is wrong; it is compile memory. test.sh / test.bat run
it next to other interop tests, so a Windows box pages: the 2026-11-07 buildci on MyCompiler ran
test_cpp_interop for 34 min (7 min alone). This was first blamed on the request-cache prune
(p2/cxx-request-cache-prune-scan-doubles-buildci); the controls below rule the cache out.

Measured 2026-11-07 (master 9bade396 on Windows, c6eb1e50 on the Mac):

| Host | Run | wall | peak |
|---|---|---|---|
| Windows | stamp flip over a full local cache (71,734 files) | 287 s | 27.34 GB working set |
| Windows | empty CFLAT_CACHE_DIR, new stamp | 273 s | 27.28 GB |
| Windows | same, `--check` | 245 s | 27.25 GB |
| Mac | empty CFLAT_CACHE_DIR, new stamp, `--check` | 93 s | 22.9 GB max RSS |

`--check` peaks the same as `-o`, so the memory is held before LLVM codegen. A warm compile (request
cache hits) stays near 6 GB.

Attribution 2026-11-07 (Codex, temporary `-v` RSS probe on mach resident_size, cold `--check`):
- 212 CxxIncrementalGroup / clang instances are created and ALL 212 are live at the peak (22.92 GB);
  every destructor runs only at exit (RSS still 22.14 GB then: allocator pages are retained).
- Each group carries its own include closure of ~1,260-1,375 headers, ~1,000-1,100 of them libc++;
  top groups grow 166-248 MB each and growth is spread across all groups (no dominant group).
- 31,407 clang Parse calls (header + request + prelude); 3.9 GB grows at group setup, 17.9 GB at
  Parse returns (per-Parse median 0, p95 32 KB, max 94 MB).
Ruled out (each breaks a constraint):
- Destroy a group once its extractions are stored: groups keep serving ParseRequest / CheckDemand /
  lazy bodies for the whole listener walk; the high-water mark (20.6 GB) is reached mid-walk.
- Rotate a group to a fresh instance after N chunks: Create re-parses the group's headers (breaks
  the cold header-parse budget).
- Drop each partial TU after extraction: all chunks share one ASTContext arena and later requests
  hold earlier declarations.

Repro (Mac):
1. `mkdir <dir>; CFLAT_CACHE_DIR=<dir> CFLAT_CACHE_BUILD_STAMP=x x64/Release/cflat --init`
2. `CFLAT_CACHE_DIR=<dir> CFLAT_CACHE_BUILD_STAMP=x /usr/bin/time -l x64/Release/cflat
   Test/test_cpp_interop.cb -i Test/library --check` -> "maximum resident set size" ~22.9e9.
Windows: scratch/box12/win_flip.ps1 (gitignored) samples PeakWorkingSet64 of the same compile.

Fix direction (design decision first): the cost is ~100 MB x one instance per header group, each
re-parsing libc++. Options: (a) fewer groups - merge header groups that share most of their closure
into one instance (check what the grouping buys: isolation of unguarded / conflicting headers);
(b) share the system-header prefix - a clang precompiled preamble / PCH of the common libc++ closure
loaded by every group (parse once, mmap); (c) lifetime analysis - know at scan time which groups get
no later requests and destroy them early. (b) keeps grouping semantics and the parse budget if a PCH
load is not counted as a header parse - needs a ruling on the budget's meaning.

Acceptance: cold test_cpp_interop peak under ~8 GB on both hosts with header-parse budgets
(cold = 1, warm = 0) and cold/warm parse counts unchanged; cold wall time not slower.
