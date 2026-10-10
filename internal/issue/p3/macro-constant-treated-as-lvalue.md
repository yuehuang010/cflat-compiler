# An imported macro constant behaves like a mutable lvalue

Summary: a C/C++ macro is a prvalue in clang, but cflat backs it with a global and lets it act as
an lvalue. Pre-existing on master; found by the T52 reviews (2026-10-06):
- `&M1` compiles (takes the backing global's address).
- `Z0 = 5;` compiles (macro globals are assignable); a later `lp(Z0)` still passes null.
- A C++ `int&` param given `N5` (`#define N5 5`) binds the backing global (clang refuses).
- `ov(N5)` with `ov(int&)` / `ov(int&&)` picks `int&` (clang picks `int&&`).
T52 (round 3) already stops the implicit address-of at a C++ pointer param.

Repros: scratch/repro_keep/t52_rev/rev4_t52_p/ and rev2_t52_p/.

Fix direction: carry the macro provenance (T52 rounds 2-4, set in ParseIdentifier) into lvalue
checks: refuse `&` and assignment on a macro, and treat a macro as an rvalue for C++ reference
binding (IsCxxRvalueReferenceArgument) - by identity, never by spelling (shadowing locals!).
