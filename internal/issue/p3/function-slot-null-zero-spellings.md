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
- `#define ZERO 0` / `0L` macros refused everywhere (clang accepts).

RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31): the accepted `function<int(int)> f = 0;` is the DEFECT, not the
rule - literal 0 is not a pointer constant in CFlat (pointer-is-not-a-number ruling 2026-09-26; the literal-0 note above), so the
declaration form is refused like the parameter, interface and generic forms. `#define ZERO 0` macros stay refused for the same reason;
only the C `NULL` macro (a null POINTER constant, T311) and `nullptr` are null spellings. Test consequence: one err_ leg for `= 0`;
the original ruling text incorrectly said no accept leg pinned it.

2026-10-31: T185 accept legs for literal 0 in testFunctionSlotAccepts flipped to err legs per this ruling (premise
'no accept leg pins it' was wrong). Literal zero was rejected for function<>, Lambda<>, and C function-pointer sinks.

Open question (mnulljoin review 1, 2026-11-01, master same): typed null pointer constants `(void*)0`, `(int*)0`, `(int*)nullptr` are
accepted at function<> slots (direct and, after mnulljoin, in joins). `(void*)0` is the C NULL body; whether a DATA-pointer-typed null
(`(int*)nullptr`) is a null spelling for a function slot needs a maintainer ruling.

Open (ctor0fn review 1, 2026-11-01): literal 0 into a C++ ctor std::function slot by DIRECT BRACE (`cppfnptr.FunctionSink v{ 0 };`,
`rv.G g{0}`) is refused only when an earlier postfix construction of the same class loaded its CFlat constructor list; as the first
construction it is accepted (the brace path goes straight to RequestCxxDirectBraceConstructor and never populates info->constructors).
Also pre-existing (master same): `rv.G g = 0;` compiles (clang: no viable conversion); `rv.G{0}` as an expression does not parse.
Repros: cflat-fix-ctor0fn scratch/rev_ctor0fn_opus_tmp/ (copied to scratch/repro_keep/ctor0fn/).
