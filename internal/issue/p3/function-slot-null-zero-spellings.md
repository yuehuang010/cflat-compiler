# Null spellings into function slots: open residue (typed null joins, C++ ctor brace, literal-0 messages)

Pre-existing, all same on master (T185 Opus r1 + r2, 2026-10-14). Repros: scratch/repro_keep/t185_rv1/ (a21, r14, a17, a16), scratch/repro_keep/t185_rv2/.
T311 fixed the C `NULL` macro item (see Test/test_function_ptr.cb testFunctionSlotNullMacro).

Settled (refusal stands, no fix item):
- Literal 0 is not a null spelling for any function slot (ruling 2026-10-31; pointer-is-not-a-number 2026-09-26).
  Refused in declaration, assignment, parameter, return, default, generic and `?:` join forms. `#define ZERO 0` and `0L`
  macros stay refused for the same reason.
- RULED 2026-10-20 (T311(b)): a `Lambda<>` value does not convert implicitly to `function<>` (doc/LANGUAGE.md Function
  types), at a plain assignment or a `?:` join alike. The spelling is `k ? p.toFunction() : nullptr` (verified, runs).
- FIXED (ternary join, the 2026-10-20 / 2026-10-31 residue): `k ? inc : 0` and `k ? lam : nullptr` were refused with a generic
  "ternary branches have incompatible types 'pointer' and 'i8'". Now refused with the null-spelling message
  ("integer constant zero is not a null spelling for a function slot; use 'nullptr'") or the closure message
  ("a closure that may carry captured state cannot become a C function pointer 'function<T>'; call .toFunction() instead").
  Same refusals, only the wording changed. Legs: Test/errors/err_function_slot_literal_zero.cb (ternary blocks at the end),
  Test/errors/err_function_slot_ternary_closure.cb.

Open, no ruling yet (stays here):
- Typed null pointer constants `(void*)0`, `(int*)0`, `(int*)nullptr` are accepted at function<> and Lambda<> slots, direct
  and in `?:` joins (mnulljoin review 1, 2026-11-01). `(void*)0` is the C NULL body; whether a DATA-pointer-typed null is a
  null spelling for a function slot needs a maintainer ruling. Repro (accepts, returns 6 at -O0 and -O2):
    int inc(int x) { return x + 1; }
    int main() { int k = 1; function<int(int)> f = k ? inc : (void*)0; return f == nullptr ? 0 : f(5); }
- Literal 0 into a function<> parameter through an interface call `i.run(0, 0)` refused with "no method matches", and into a
  generic T = function<> parameter (`isNull<function<int(int)>>(0)`, `Box<function<..>>.set(0)`) refused with "no overload
  matches". Refused as ruled; the wording does not name the null-spelling rule. No ruling on the wording.

Open (ctor0fn review 1, 2026-11-01): literal 0 into a C++ ctor std::function slot by DIRECT BRACE (`cppfnptr.FunctionSink v{ 0 };`,
`rv.G g{0}`) is refused only when an earlier postfix construction of the same class loaded its CFlat constructor list; as the first
construction it is accepted (the brace path goes straight to RequestCxxDirectBraceConstructor and never populates info->constructors).
Also pre-existing (master same): `rv.G g = 0;` compiles (clang: no viable conversion); `rv.G{0}` as an expression does not parse.
Repro shape: a C++ class with a std::function<int(int)> converting constructor (Test/library/cpp_interop_fnptr.h FunctionSink);
the first statement constructing it is `FunctionSink v{ 0 };`. Second root cause (brace path skips the constructor list), so not in
the ternary fix.
