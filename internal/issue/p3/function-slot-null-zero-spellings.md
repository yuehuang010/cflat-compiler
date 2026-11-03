# Null spellings into function slots refused where clang / direct calls accept (remaining: ternary join)

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
- Fixed in MIX20: `Lambda<int(int)> f = MNULL;` and `g = MNULL;` (Lambda<> declaration / assignment) now store a null.
  CreateCast's aggregate backstop (LLVMBackend_VariablesAndIR.cpp) takes a load of a zero macro global as nullptr for
  closure destinations only. Legs: Test/test_function_ptr.cb testLambdaSlotNullMacro; Test/errors/err_lambda_nonnull_pointer_global.cb.
- `k ? MNULL : MNULL` into function<> refused; `k ? nullptr : nullptr` accepted; clang accepts both.
  Root cause (MIX20): ArgumentIsProvablyDataPointer exempts a C null macro only for a direct load of the macro global
  (arg.IsCxxMacroGlobal). A `?:` join carries IsCxxMacroJoin instead, so JoinDeliversDataValue reads the arms as data
  and the thin gate (CheckThinFnPtrAssignProvenance) refuses. The Lambda<> form is refused for the same select shape.
  Fix needs an all-arms-zero-macro proof on the join in the thin gate and the fat backstop; not one site, not done.
- `#define ZERO 0` / `0L` macros refused everywhere (clang accepts).
- `import cpp`: `callit(nullptr)` into a std::function / fn-pointer param -> "no overload"; NULL accepted.

RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31): the accepted `function<int(int)> f = 0;` is the DEFECT, not the
rule - literal 0 is not a pointer constant in CFlat (pointer-is-not-a-number ruling 2026-09-26; the literal-0 note above), so the
declaration form is refused like the parameter, interface and generic forms. `#define ZERO 0` macros stay refused for the same reason;
only the C `NULL` macro (a null POINTER constant, T311) and `nullptr` are null spellings. Test consequence: one err_ leg for `= 0`;
the original ruling text incorrectly said no accept leg pinned it.

2026-10-31: T185 accept legs for literal 0 in testFunctionSlotAccepts flipped to err legs per this ruling (premise
'no accept leg pins it' was wrong). Literal zero was rejected for function<>, Lambda<>, and C function-pointer sinks.

Open (2026-11-01, fnzcpp review 2): literal 0 into a C++ CONSTRUCTOR std::function parameter (`S2(std::function<int(int)>)`,
`S(std::function<int(int)>, int)` with `S(0, 5)`) is still accepted; the free / member call is refused. The ctor path appears to
convert the argument before RejectLiteralZeroFunctionSlot sees it (LLVMBackend_CxxLowering.cpp EmitCxxStructorCall).
Repro: cflat-fix-fnzcpp scratch/rev_fnzcpp_r2/ctorsf1.cb (kept in scratch/repro_keep/fnzcpp/).
