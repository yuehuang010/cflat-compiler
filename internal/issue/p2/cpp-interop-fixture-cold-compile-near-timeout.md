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
