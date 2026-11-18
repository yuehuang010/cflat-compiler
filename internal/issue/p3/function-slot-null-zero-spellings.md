# Null spellings into function slots: open residue (typed null joins, literal-0 messages)

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
- FIXED (wording, ruled 2026-11-16 from precedent): literal 0 at a function<> parameter of an interface call (`i.run(0, 0)`) or a
  generic T = function<> call (`isNull<function<int(int)>>(0)`, `Box<function<..>>.set(0)`) reports the null-spelling message at the
  argument. Same refusals. Legs: Test/errors/err_function_slot_literal_zero.cb (last three blocks).

RULED 2026-11-18 (maintainer): REFUSE ALL THREE cast spellings - `(void*)0`, `(int*)0`, `(int*)nullptr` are not null
spellings for a function<> or Lambda<> slot, direct or in a `?:` join, as C++ (a void* / data pointer does not convert
to a function pointer). `nullptr` and the C `NULL` macro (T311) stay. Rejected: keeping `(void*)0` as the C NULL body
(the main session's pick) - where C and C++ disagree, C++ wins. Test consequence: the repro below becomes an err_ block
in Test/errors/err_function_slot_literal_zero.cb for each spelling x {declaration, assignment, `?:` join}; message names
`nullptr`; `NULL` legs in testFunctionSlotNullMacro stay green.

Question as filed (was: Open, no ruling yet):
- Typed null pointer constants `(void*)0`, `(int*)0`, `(int*)nullptr` are accepted at function<> and Lambda<> slots, direct
  and in `?:` joins (mnulljoin review 1, 2026-11-01). `(void*)0` is the C NULL body; whether a DATA-pointer-typed null is a
  null spelling for a function slot needs a maintainer ruling. Repro (accepts, returns 6 at -O0 and -O2):
    int inc(int x) { return x + 1; }
    int main() { int k = 1; function<int(int)> f = k ? inc : (void*)0; return f == nullptr ? 0 : f(5); }

Open (ctor0fn review 1, 2026-11-01): `rv.G{0}` as an expression still does not parse; this grammar form remains out of scope. The
declaration form `rv.G value{0};` and copy-init form `rv.G value = 0;` are covered by the FNZERO fix. The remaining inline repro is:
```cflat
extern int main() { rv.G{0}; return 0; }
```
