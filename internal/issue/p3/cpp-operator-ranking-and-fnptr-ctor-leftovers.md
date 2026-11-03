# C++ operator ranking leftovers after MIX7

Found: MIX7 r1 opus review, 2026-10-30 box 7, PRE-EXISTING (same on master). Probes scratch/repro_keep/mix7/mix7_rev/ (main checkout).
- `<=>` rewrite `d < 1` gives 83, clang 84; unary `~u` gives 71, clang 72 (prefers global non-const `U&` overload).
- The namespace+global joint ranking only looks at the LEFT operand's class: `1 + x` (scalar lhs, class rhs) keeps the old lookup.
  MIX14 re-probed (scratch/mix14_tmp/r14.cb, namespace nsx operator+(long, const X&) vs global operator+(int, const nsx::X&)):
  clang 12, master and MIX14 11.
- Root cause (MIX14): binary/compound free-operator lookup keys its namespace-qualified name on the LEFT class only
  (MainListener_Expressions.cpp ~21679, ~21955, ~22177 take `dot` from the lvalue's type name), so a scalar lhs never
  reaches the right operand's namespace. Not fixed in MIX14 (not attempted).
- Root cause (MIX14): `<=>` and `~u` reproduced unchanged (cmp 83, U 71); the rewritten-candidate and unary joint ranking
  were not investigated in MIX14.
- Fixed in MIX14 (fnptr ctor): `tv.Ob(twice)` with a function pointer to a C++ `Ob(bool)` constructor now takes the
  pointer's truth (LLVMBackend_CInterop.cpp EmitCxxStructorCall); leg in Test/test_cpp_interop.cb (65055).
- Observed (MIX14, master too): `printf("%d", ((tv.Ob)(twice)).v)` in a variadic argument list refuses with
  "cannot store a pointer value into struct storage" (a LogError, not a crash); the declaration form works.
- Fixed in MIX14 r2: a CFlat function into a by-value C++ `int` ctor parameter (`rv.CI(tw)`) is refused (was a module
  verification failure); err_cpp_fnptr_into_int_ctor.cb.
- Fixed in MIX18: free functions `a1(const double&)` + `a1(long)` (also `+ a2(float)`, `+ a11(unsigned)`) called with an
  int now take the by-value overload (ruling; clang reports these ambiguous, the ruling diverges by design). Root cause:
  the C++ conversion-rank pass left a `const T&` scalar unjudged (CxxIntegerParameterIdentity is empty for a reference),
  and the tier pass gave a floating referent integer cost -1, which the tier keepLowest ranks best. Now judged as its
  referent (RankCxxConversionSequences) and costed as a conversion (the tier pass paramIdentity, LLVMBackend_Overloads.cpp ~3773). `a7(const double&)` +
  `a7(const long&)` now refuses as ambiguous like clang (test: Test/errors/err_cpp_const_ref_scalar_tie.cb); master picked
  the const double& silently (MIX18 brief said "stays ambiguous" - it did not on master). Legs: Test/test_cpp_interop.cb
  65057-65059, header Test/library/cpp_mix18_overloads.hpp.
- Fixed in MIX18: `rv.CBI(tw)` (`CBI(bool)` + `CBI(int)`, a CFlat function) picks bool: a pointer or function value is
  non-viable at a non-bool arithmetic parameter in the C++ conversion-rank pass (pointer -> integer needs a cast).
  `rv.CB(tw)` into `const bool&` is its truth (clang 1): the ctor compatibility check accepts a function value for a bool
  target, and the lowering materializes the truth in a temporary (EmitCxxStructorCall). The cast form `((rv2.CBV)(p))`
  with a pointer variable now direct-initializes (MainListener_Expressions direct-init branch takes pointer sources).
- Root cause (MIX18, unfixed): the cast form with a FUNCTION operand, `((rv.CB)(tw)).v` and `((rv.CBI)(tw)).v`, still
  refuses "cannot store a pointer value into struct storage". The direct-init branch (ParseCastExpressionImpl, the
  pointerSource check) is never reached for a function designator operand; the cast falls through to the aggregate
  bitcast backstop (LLVMBackend_VariablesAndIR.cpp ~1438). Not traced past that branch; the pointer-variable cast works.
- Fixed in MIX18 r2: free function `a3(const bool&)` + `a3(int)` with a CFlat function value (`rv.a3(tw)`, clang 1) takes
  the truth. The scalar-reference arm of the candidate loop scores a function value into a `const bool&` as a conversion
  (result 1), the code-value refusal exempts that parameter, and the free-call lowering materializes the truth
  (LLVMBackend_Overloads.cpp, the pointerBoolReferent branch). Leg: Test/test_cpp_interop.cb 65064 (header a3 overloads).
- Fixed in MIX18 r2: a method `s.m(i)` (`m(const double&)` + `m(long)`) and a member operator `s + i` (`operator+(const double&)`
  + `operator+(long)`) pick the by-value overload for an int (ruling). The const-ref count is now computed from the candidate's
  own parameters (keepFewestConstRefTemporaries) with the same predicate as the rank tie-break (CxxConstScalarBindsTemporary,
  CxxConversionRank::constRefTemporary). Legs: Test/test_cpp_interop.cb 65061-65062. Two const T& temporaries still tie
  (Test/errors/err_cpp_const_ref_scalar_tie.cb, err_cpp_const_ref_ctor_tie.cb).
- Root cause (MIX18 r2, unfixed): a CONSTRUCTOR `S(i)` with `S(const double&)` + `S(long)` (and `U(i)` with `U(const long&)` +
  `U(double)`) is still refused (clang: ambiguous). The postfix construction path selects with allowNumericConversions=false, so
  a `const double&` candidate is not viable there, and CxxConstructorNeedsClangResolution hands every scalar-reference
  constructor set to clang (CxxCtorForwardsScalarReference), which reports the tie. The ruling needs CFlat to decide such sets:
  a policy change to the clang deferral plus numeric-off viability, not a tie-break alone.
- Observed, not MIX18 scope: `rv.CB(d)` (double into `const bool&`, clang 1) and `rv.CNI(d)` / `rv.CNI(l)` (double/long
  into `const int&`, clang 2 / 6) are refused: cflat refuses the implicit narrowing/float->bool at a call (rulings
  2026-09-04 narrowing, 2026-10-13 float->int); decide before any change.
- RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31), MIX18 constructor sets: a C++ CONSTRUCTOR follows the same
  by-value-beats-const&-temporary rule as free functions, methods and operators (967d0a9a) and the MIX11 line at
  LLVMBackend_Overloads.cpp ~4528 (dffcb4b7): `S(i)` with `S(const double&)` + `S(long)` picks `long`, `U(i)` with `U(const long&)` +
  `U(double)` picks `double`; only an lvalue of exactly the referent type ties, and two const& temporaries stay ambiguous. CFlat decides
  such sets itself: the clang deferral (CxxConstructorNeedsClangResolution / CxxCtorForwardsScalarReference) and the numeric-off
  viability no longer apply to a scalar-reference candidate set. Test consequence: `S(i)` / `U(i)` become accept legs next to 65057-65062;
  err_cpp_const_ref_ctor_tie.cb (two const& temporaries) stands. The narrowing items (`rv.CB(d)`, `rv.CNI(d)`) stay refused by the
  2026-09-04 and 2026-10-13 rulings - no change.
