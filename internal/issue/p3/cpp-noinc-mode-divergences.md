# Non-incremental C++ request mode still refuses what the incremental executor binds

**Summary.** Keep `CFLAT_CPP_INCREMENTAL=0`; the fallback must bind what the default incremental executor binds.
Both fallback crashes are fixed (GEP APInt width compare, landed 33e7a93c; Clang CodeGen over an error expression in a
constructor member initializer), so only refusals remain - p3 per the ruling.

**Switch.** `UseCxxIncrementalRequests()` in `cflat/LLVMBackend_CInteropShared.h` reads `CFLAT_CPP_INCREMENTAL`: unset
enables the incremental executor; `0`, `off`, or `false` disables it. Not documented in `doc/CACHING.md` or `doc/CLI.md`.
No suite leg runs the fallback.

**Repro.** Fresh `CFLAT_CACHE_DIR`, `-i Test/library`, `-o`, `CFLAT_CPP_INCREMENTAL=0`.

Fixed by NOINC (fix box 16), each measured on master and on the fix:
- calloc: a fallback request TU walks its whole include graph, so every type request re-harvested libc's `::calloc`
  (out of the import's scope, size_t spelled `unsigned long`) and the next `calloc((i64)n, ...)` call in core/list.cb
  was ambiguous with core's `calloc(win_usize, win_usize)`. Fallback requests now drop plain free functions their
  headers declare (an incremental chunk never re-harvests them), after the PCH-retry judgement.
- `struct Holder { std.unique_ptr<cppon.Cls> p = default; }` + `h.p = std.unique_ptr<cppon.Cls>(new cppon.Cls(7));`:
  "selected deleted operator '='" (the inline move assignment had no emitted body). Assignment now requests a refused
  operator= body on use in the fallback (EnsureCxxAssignmentProjected), named-variable and struct-field paths.
- `[cpp] struct E : std.enable_shared_from_this<E>`: "base ... is not a C++ class". Two causes: the extractor's
  operator= overload probe (AssignmentSelectsConstRef) left a protected-access error as the TU's first clang error
  (now suppressDiagnostics), and the fallback ODR-used the protected destructor (now skipped).

Fixture first stops (rc 1 = compile refused), master -> fix:
- `Test/test_cpp_interop_bridge.cb`: calloc (list.cb:48, reported as bridge 48,31) -> 1013 -> 3504 (below).
- `Test/test_cpp_interop_template.cb`: 423 enable_shared_from_this -> 431 (below).
- `Test/test_cpp_interop.cb`: calloc (reported as application.cb 48,31) -> 282 (below).

## Open (measured 2026-11-19 on the NOINC exe)

- `Test/test_cpp_interop.cb` 282: "C++ header 'cpp_interop_demand_fail.h' does not compile (clang: use of undeclared
  identifier 'undefined_x' at cpp_interop_demand_fail.h:97)". Master noinc fails the same way on a one-line probe
  (`import cpp "cpp_interop_demand_fail.h"; int main() { return 0; }`); default mode exit 0. Root cause: the import
  harvest uses demand-only definitions (`req.demandOnlyDefinitions`, LLVMBackend_CHeaderDemand.cpp) only when the
  incremental executor is on, so the fallback parses the header's broken inline bodies (T424Demand) eagerly and
  refuses the whole header. Fix needs a deferred-body path for the one-shot import.
- `Test/test_cpp_interop_template.cb` 431: "the base 'cppi_crtpf.Base' of struct 'CrtpFieldSelf' is not a C++ class".
  Probe: `import cpp "cpp_interop_crtp_field.h"; [cpp] struct S : cppi_crtpf.Base<S> { int value = default; };`
  (master and fix noinc refuse, default exit 0). `-v`: "could not be parsed: field has incomplete type
  '__cflat_user::cf13x...'" - the stage-1 request over the still-incomplete [cpp] struct completes a member signature
  naming it by value (`std::pair<D, D> makep()`); the incremental path defers incomplete-argument codegen.
- `Test/test_cpp_interop_bridge.cb` 3504,0 (`struct SharedHolderNet : cppt.ModuleBase` with a
  `std.shared_ptr<SharedHolderBlock>` field): "use of undeclared identifier '__cflat_user'" - same gap as the
  make_shared item below (a one-shot TU that names a [cpp] struct it does not declare). The earlier 1013 stop
  (std.list<int> field copy-assign, "no usable assignment operator") is fixed by the operator= retry.
- enable_shared_from_this probe (`std.make_shared<E>()` then `shared_from_this()`): the base binds now; the next stop
  is "C++ free function 'std.make_shared' could not be bound (clang: use of undeclared identifier '__cflat_user')" -
  the fallback's wrapper TU (RequestGeneratedCxxWrapper) does not declare the [cpp] struct the wrapper names; the
  incremental TU has it from an earlier chunk.
- static_assert item: the filed accept case (`int v = (fail<T>(), 42);`) does not reproduce. Five variants (plain
  and class-template default member initializer, user ctor, CFlat field holder, function template) refuse in both
  modes on master and fix. Remaining divergence is wording only: the fallback relays the static_assert text without
  the "clang: " prefix (`sa_dmi.cb(2,21): static assertion failed ...` vs `clang: static assertion failed ...`).

RULED 2026-10-31 (maintainer): KEEP the non-incremental switch; the divergence is a real defect to FIX, not a path to
delete - the fallback must bind what the incremental executor binds. Severity: p3 while the fallback only refuses; a
CRASH in the fallback is p2.
