# Null spellings into function slots refused where clang / direct calls accept (remaining: literal 0 and a ternary join)

Pre-existing, all same on master (T185 Opus r1 + r2, 2026-10-14). Repros: scratch/repro_keep/t185_rv1/ (a21, r14, a17, a16), scratch/repro_keep/t185_rv2/.
T311 fixed the C `NULL` macro item (see Test/test_function_ptr.cb testFunctionSlotNullMacro). Remaining:
- `k ? inc : 0` into function<> refused ("incompatible types 'pointer' and 'i8'"). Literal 0 is not a pointer
  constant in CFlat by design (pointer admission dropped); do not make literal 0 convert.
- Literal 0 into a function<> parameter through an interface call `i.run(0, 0)` -> "no method matches" (direct call accepts).
  Same literal-0 rule; leave as is.
- Literal 0 into a generic T = function<> parameter (`isNull<function<int(int)>>(0)`, `Box<function<..>>.set(0)`) ->
  "no overload matches"; nullptr and named functions work. Same literal-0 rule.
- RULED 2026-10-20 (T311(b)): keep refused. `function<>` is thin and never carries an env; a `Lambda<>` value does not
  convert implicitly to `function<>` (doc/LANGUAGE.md Function types), at a plain assignment or a `?:` join alike. The
  spelling is `k ? p.toFunction() : nullptr` (verified, runs). Not a fix item.
Fix direction for the literal-0 items: none in this issue (literal 0 is out of scope by design).

## After T311 review 1 (2026-10-16, all same on master)
- `Lambda<int(int)> f = MNULL;` and `g = MNULL;` (Lambda<> declaration / assignment) still refused "cannot store a pointer
  value into struct storage" (LLVMBackend_VariablesAndIR.cpp ~1322-1328 only admits a real null constant); a Lambda<>
  PARAMETER accepts the macro after T311. Fix direction: substitute a real null at those two sites when the RHS is the
  ilz macro global load (same test as LLVMBackend_WinRT.cpp ArgumentIsProvablyDataPointer). Repro scratch/repro_keep/wt_t311/rev_t311_repro/lambda_null.cb.
- `k ? MNULL : MNULL` into function<> refused; `k ? nullptr : nullptr` accepted; clang accepts both.
- Literal `function<int(int)> f = 0;` IS accepted (contradicts the literal-0 note above - check against the ruling);
  `#define ZERO 0` / `0L` macros refused everywhere (clang accepts).
- `import cpp`: `callit(nullptr)` into a std::function / fn-pointer param -> "no overload"; NULL accepted.
