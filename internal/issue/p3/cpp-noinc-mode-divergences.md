# Non-incremental C++ request mode still refuses what the incremental executor binds

**Summary.** Keep `CFLAT_CPP_INCREMENTAL=0`; the fallback must bind what the default incremental executor binds.
Both fallback crashes are fixed (GEP APInt width compare, landed 5feb5d63; Clang CodeGen over an error expression in a
constructor member initializer, fixed with this issue's move to p3), so only refusals remain - p3 per the ruling.

**Switch.** `UseCxxIncrementalRequests()` in `cflat/LLVMBackend_CInteropShared.h` reads `CFLAT_CPP_INCREMENTAL`: unset
enables the incremental executor; `0`, `off`, or `false` disables it. Not documented in `doc/CACHING.md` or `doc/CLI.md`.

**Repro.** Fresh `CFLAT_CACHE_DIR`, `-i Test/library`, `-o`, `CFLAT_CPP_INCREMENTAL=0`. First stops:

- `Test/test_cpp_interop.cb` line 92,24: "cannot choose which 'calloc' to call: calloc(u64, u64), calloc(ulong, ulong)"
  (default mode binds one calloc).
- `Test/test_cpp_interop_template.cb` line 418: "the base 'std.enable_shared_from_this' of struct 'CrtpEsfOwned' is not
  a C++ class".

- Accept-vs-refuse: a body whose only error is a failed `static_assert` (no invalid record, no error expression) is
  not caught by `ErrorReachScan`, so `template<class T> void fail() { static_assert(sizeof(T)==3, ""); }` reached from
  `int v = (fail<T>(), 42);` compiles and runs (42) in the fallback; default mode and clang refuse. Probe cases
  `dmi` / `dmi_only` in the box-9 Sol review script (scratch/rev_noinc_sol_probe.sh in the noinc worktree).

Default incremental runs of both fixtures compile and run.

**Landed so far.** Fallback-only nested member type completion (`sizeof(owner)` before the marker), shared
special-member records and scoped completion results, a use-site retry that ODR-uses the requested member body
(including unary operators and refused base members) and restores request state when the retry fails, no
`CxxIncrementalGroup` created by one-shot type requests, and the CodeGen crash fix: `ErrorReachScan` (CClangExtract.cpp)
now walks constructor member initializers and in-class default member initializers (CXXDefaultInitExpr), so `M(const M& o) : v(o.v + Bad<T>::value)` (a failed static_assert) and
the implicit copy constructor reaching it are emptied instead of handed to CodeGen. No suite leg runs the fallback.

RULED 2026-10-31 (maintainer): KEEP the non-incremental switch; the divergence is a real defect to FIX, not a path to delete - the
fallback must bind what the incremental executor binds. Severity: p3 while the fallback only refuses; a CRASH in the fallback is p2.
Fix direction stands (diff the `-v` member harvest of `std::map<...>::find` between the two modes, align the request-TU completion.
