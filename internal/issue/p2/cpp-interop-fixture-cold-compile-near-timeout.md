# test_cpp_interop cold compile runs at the 480 s heavy-test limit under load

Found 2026-10-11 (fix box). Cold compiles of Test/test_cpp_interop.cb (fresh header cache, e.g. the
first landing bar after a kCHeaderCacheVersion bump or in a new worktree) took 300-476 s at load
8-12 on macOS arm64 (132-165 s on quieter runs 10-09/10-10); test.sh's HEAVY_TIMEOUT_SECS is 480.
Three landing bars (T101, T103, T107) failed "FAIL compile" with output cut at the same buffered
point (line 12196); the standalone compile passed each time (warm compile: 0 s).

-ftime-trace of a cold compile (T107 tree, 448 s, load ~11), top phases:
- CodeGeneration 241 s (21 events)
- BaselinePasses 173 s (1 event - LLVM baseline pipeline on the whole module)
- Source 37 s; RequestGeneratedCxxWrapper 29 s (1823); CxxRequestGroup 26 s (5771);
  CxxTypeRequestJsonLoad 23 s (22333 loads)
So ~93% is LLVM backend work on one very large module, not header parsing.
Trace: scratch/repro_keep/interop_cold_trace/test_cpp_interop.time-trace.json.

Mitigation in place (main session): scratch/bar_wt.sh pre-warms the worktree header cache with one
test_cpp_interop compile before test.sh. The test and its limit are unchanged.
Fix direction: split the fixture (it is one translation unit of ~12k+ lines - codegen and the
baseline pipeline scale with module size) or find why BaselinePasses is superlinear on it; measure
on a quiet machine first (load inflates this 2-3x).

Root cause (2026-10-13, master 7847a00df): NOT header work and NOT a cold cache - a WARM test.sh run
spent 494 s in this one compile (user CPU, load-independent). Two superlinear costs in `main`:
1. `OwnedTempDominatesHere` (71% of samples): every query scanned the whole function
   (`FunctionIsWellFormed`) and, mid-codegen, ran a forward CFG walk from entry with a fresh
   unordered_set -> O(statements x blocks). Fixed in the working tree: backward walk from the
   query block, statement-local (494 -> 285 s, suite 604 -> 523 s, 1401/0/8).
2. Landing-pad duplication: `CreateCallOrInvoke` emits a fresh pad per invoke that re-destroys
   EVERY live object in every enclosing scope. `main` has 3359 invokes x ~155 dtors = ~520k of its
   548k calls; SROA/mem2reg and codegen pay O(invokes x live objects). Step change 81 -> 404 s came
   with T70 (a79abdf25, 10-09 20:10).
Fixed on macOS/Linux (perf box 2026-10-13, includes fix 1): Itanium pads chain per-LOCAL cleanup
regions deduped by structural hash (positional compare for in-region values and scratch allocas
from `AllocaAtEntry`, WeakVH cache so trial codegen erasing blocks is safe), one exn slot + resume
per function; `return` with >= 4 live locals stores the value + a destination id and jumps into a
shared cleanup chain ending in a switch (clang's cleanup.dest). `main` calls 548k -> 8451; compile
494 -> 25 s; test.sh 729 -> 136 s warm; test_lsp 266 -> 30 s; 1401/0/8, leak multiset of the
fixture unchanged.
OPEN (why this file stays): the Windows funclet path keeps per-pad duplication - needs nested
cleanuppads (`cleanupret unwind to` parent); test_cpp_interop on Windows still pays it.
Splitting the fixture is not the fix.
