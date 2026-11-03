# Non-incremental C++ request mode diverges and crashes

**Summary.** The ruled non-incremental request executor must bind what the default incremental executor binds. It first refused a nested member typedef in `Test/test_cpp_interop_template.cb`; after that fix, it still loses template member bodies. `Test/test_cpp_interop.cb` crashes in fallback mode on both the worktree and frozen master binary, so severity is p2.

**Switch.** `UseCxxIncrementalRequests()` in `cflat/LLVMBackend_CInteropShared.h` reads `CFLAT_CPP_INCREMENTAL`: unset enables the incremental executor; `0`, `off`, or `false` disables it. No entry was found in `doc/CACHING.md` or `doc/CLI.md`.

**Repro / first stops.** Both fixture commands use `-v`, `-i Test/library`, `-o`, and a fresh `CFLAT_CACHE_DIR` per mode.

- Before the change, non-incremental `Test/test_cpp_interop_template.cb` stopped at line 579: `cannot find the type 't37.vec<int>.value_type'`; default mode linked successfully.
- After the change, the next non-incremental stop is line 329: `no overload of 'operator[]' matches the given arguments.` The only candidate is `operator[](KeyBag*, KeyIdx)` for a `cppt.Box<int>` receiver and `i8` index. `cppt.Box<int>::operator[]` is harvested without a body.
- Non-incremental `Test/test_cpp_interop.cb` exited 139 with a compiler crash dump ending at `operator[](list<string>*, int)`. The exact same command against `/Users/felixhuang/source/cflat-compiler/x64/Release/cflat` also exited 139. The crash prevents enumerating later divergences in that fixture.

**Root cause and worktree change.** The fallback reparses each request in a fresh TU, so nested specialization completion and some instantiated bodies do not carry across requests as they do in the live interpreter. For nested template member types, `RunCxxTypeRequests` now emits a fallback-only `sizeof(owner)` completion before the marker; the fallback binds the exact scalar marker even when completion added owner records. Its stage-2 ODR-use emits a body for nontrivial destructors. Incremental request source construction and member-use rules remain unchanged. `cppt.Box<int>::operator[]` remains unbound, so the template fixture is still divergent and `std::map::find` has not yet been reached.

**Tests.** No test or script was added: current suite scripts have no non-incremental leg, and existing test legs are read-only. Continue this issue by aligning fallback member-body harvest, then rerun both fixture modes.
RULED 2026-10-31 (maintainer): KEEP the non-incremental switch; the divergence is a real defect to FIX, not a path to delete - the
fallback must bind what the incremental executor binds. Severity: p3 while the fallback only refuses; a CRASH in the fallback is p2.
Fix direction stands (diff the `-v` member harvest of `std::map<...>::find` between the two modes, align the request-TU completion).
