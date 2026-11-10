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

## Request phase: fan-out probes copy a dependency's libc++ closure into every group (2026-11-08)

Summary: during the cold request phase of Test/test_cpp_interop.cb, RSS grows ~13 GB with no new
"clang parse:" lines. Nearly all of it is prelude include parses inside existing groups
(CxxIncrementalGroup::ParseRequest, the "Commit the leading #include lines" block): a request
whose group carries dependency headers includes them into the primary group's interpreter for
good. Fan-out probes do this to EVERY group, so each of ~210 interpreters re-parses the same
dependency closure (cpp_interop_basic.h -> <functional>/<map>/<mutex>/<vector>/...). This is the
mechanism behind the 17.9 GB "at Parse returns" above.

Measured 2026-11-08 (Mac, Release, fresh CFLAT_CACHE_DIR, `-o`, temporary phys_footprint probes):
- 35,155 ParseRequest calls, 216 groups. Request chunk Parse: ~0.2 GB total; extraction ~0.1 GB;
  harvested request bitcode memoized in wrapperResults / typeResults: 0 MB (EDEF requests defer
  bodies to the demand pass). Prelude include Parse: 1,070 calls, 13.4 GB.
- By header: cpp_interop_basic.h included into 207 groups = 12.9 GB (~62 MB each); all others
  < 0.3 GB combined.
- Sources: infix / free-function probes in LLVMBackend_CxxRequestFunctions.cpp (candidateGroups
  = dependency groups + CandidateCxxGroupsFor, which returns ALL groups incl. "rest") with
  dependencyGroups = {basic.h}; 11 calls ran the full 214-group loop and failed everywhere
  (cppi::operator<<, cppi_inh::operator+/-, cpp_errc_probe::operator==, t460::f), 2 found late
  (std::setw 164, std::noop_coroutine 97, no deps).
- Prototype (not kept): skip groups that neither are a dependency nor declare the lead namespace
  nor publish the name -> prelude parses 1,070 -> 484, program output identical, but the cost
  moved: the `t518::pick<t518::Inc>` type request then fans to 211 groups carrying
  cpp_interop_lastuse.h -> twin.h -> basic.h (11.5 GB); peak footprint unchanged (21.45 GB).
  Every fan-out site with a basic.h-reaching dependency pays the same ~62 MB x groups once.
- Measure with `/usr/bin/time -l` "peak memory footprint", not max RSS: max RSS swings 16-22 GB
  run to run under memory compression, footprint stays 21.4-21.5 GB.
- Rejected: resetting each request PTU's llvm::Module after harvest (safe - nothing reads it, the
  extractor ignores it) but peak footprint 21.54 GB vs master 21.53 GB; its "17.7 GB" was RSS.

Owner: C++ request routing (LLVMBackend_CxxRequestFunctions.cpp free/infix loop,
LLVMBackend_CxxRequestTypes.cpp type/template fan-out, MakeCxxRequestGroup,
CxxIncrementalGroupKey keyed on ownerHeaders only).

Fix direction (needs a ruling; each option changes which TU a probe runs in):
(a) narrow fan-out candidates to groups that can name the lead namespace / published name - one
    rule for all fan-out sites, else the cost moves (prototype above);
(b) run a fan-out probe in the DEPENDENCY group's interpreter with the primary's headers appended
    (small headers, libc++ already there) - include order differs from the cache key's;
(c) shared libc++ preamble / PCH per group (option (b) above) - removes the
    per-group copy for every cause at once.
Negative fan-out results are also not memoized: the same wrapper ran the 214-group loop twice.

Acceptance: cold test_cpp_interop peak under ~8 GB with program output, cold/warm clang parse
counts and `--error-on-cpp-reparse` unchanged.

## Ruling (maintainer, 2026-11-08)

- No shared libc++ preamble / PCH across groups (option (b) above and option (c) of the request-phase
  section are rejected): CFlat makes no assumption that a system closure can be shared. Each import is
  already a module (PCH-like unit).
- Deduplicate imports instead: when main.cb imports A and B and A itself imports B, the B module is
  loaded once and reused - never re-parsed or re-included into another group. The request-phase
  fan-out (cpp_interop_basic.h included into ~207 groups) is the same defect: a dependency that is
  already its own module must be reached through that module, not copied into every group.
- Target: a warm compile peaks ~3.5 GB, so once a cold compile has finished parsing, its memory must
  come back down to about that level - cold-only work (request probes, failed fan-out attempts) must
  not stay resident for the rest of the compile.


## Rehosting requests into the dependency's TU: measured, rejected (memdedup, 2026-11-08)

- Tried: serve a request with dependency groups in the live TU of the member missing the fewest
  closure files (MakeCxxRequestGroup -> ownerHeaders moved, `primary` kept), plus weak_odr for
  strong definitions from headers outside a group's own imports.
- Measured (Mac Release, fresh cache, `-o`, peak footprint): test_cpp_interop cold 21.0 -> 9.7 GB,
  191 -> 111 s, parses 228 = 228, output identical; template cold 5.60 -> 3.75 GB. Fix-cold
  plateau after parsing still ~7.2 GB (216 live interpreters kept for the demand pass).
- Rejected (Sol review): the primary's headers are then parsed AFTER the host's, so lookup and
  preprocessing change - an overload or a macro the dependency defines flips A's template
  (master / clang 11, rehost 22). weak_odr also left a sole strong extern "C" definition
  weak, which ODR cleanup internalized: undefined symbol for C code taking its address.
- A provably equivalent host must already have parsed the primary's headers first, i.e. an
  import line starting with them: 2 grouped lines of 215 in test_cpp_interop, never the
  basic.h fan-out. So no safe variant of this keeps the win. Directions left: shrink the probe
  fan-out itself (fewer candidates, negative memo), or lifetime-bound interpreters.

## 2026-11-09 - release + movedf sets landed, fan-out pruning parked (fix/memfan)

- Landed (pick-neutral): interpreters released right after LinkCxxCompanionModules plus
  mi_collect(true) (mimalloc kept the freed pages); movedf sets as sorted interned ids
  (FlowSet) instead of per-block string-set copies (+2.3 GB transient -> +0.2 GB, MOVE-DF verify
  diff 0). test_cpp_interop cold 22.56 -> 20.33 GB, 128.8 s, parses 228; warm 3.69 -> 1.47 GB.
- PARKED: fan-out pruning (skip a candidate whose own harvested TU lacks the lead, define sets
  equal), patch scratch/repro_keep/memfan/pruning_r3.patch, cold 9.13 GB. Reason: a candidate
  without the lead still changes a dependency's template body (its headers come first) - Sol
  gap_callable: master/clang 11, pruning 22, cold and warm. A header text scan was rejected
  earlier (newline/comment after `namespace`, macro and pasted namespaces).
- Also blocking the skip when a dependency group declares the full name or a non-namespace
  prefix (qualified namespace-member names harvested) measured 20.23 GB: the one remaining
  fan-out, t518::pick, is owned by the dependency cpp_interop_lastuse.h, 189 candidates kept.
- Dropped: a negative memo for all-failed free fan-outs - no measured gain (20.32 vs 20.33 GB)
  and its invalidation against later prelude commits is unproved.
- Left: ~3.1 GB header ASTs + request-phase interpreters live until the last request; a sound
  prune needs a proof over the candidate+dependency TU, not the candidate's own harvest.
