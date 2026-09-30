# P3 C++ constructor ranking leftovers after C6 round 2

## Summary

Two diagnosed C++ interop gaps remain outside C6 round 2. The ranking fixes for
bool literals and clang delegation from `new` are handled separately.

## Repros

1. The pre-fix `scratch/rev1/` repro `new rv.NF(1.5)` was refused with a
   diagnostic that called the unsuffixed double literal `float`; clang chooses
   `NF(double)` (chosen=5). C6 round 2 now accepts that shape through clang, but
   the refusal formatter still derives the displayed type from the narrowed
   LLVM constant. Fix the diagnostic at its source for any remaining refusal.
2. In `scratch/rev1/m_D_enum.cb` and related enum cells, passing the unscoped
   `rv.Green` enum to `rv.D` is rejected as ambiguous in declaration,
   expression, and `new` forms, while `clang++ -std=c++20` chooses `D(int)`
   (chosen=5).
3. Needs ruling: float -> int in C++ ctor args, `new` vs decl/expression asymmetry. Do not
   change declaration or expression forms until the language rule is settled.

## Root cause / direction

1. `TypeUntypedCtorArg` and refusal formatting narrow or describe the literal
   from its LLVM constant type. Preserve source literal identity in the error
   path so an unsuffixed decimal reports double. Do not change overload choice.
2. The enum argument reaches C++ constructor selection in a form that permits an
   ambiguous result rather than applying the standard enum-to-int promotion.
   Trace the shared selection path and compare all three syntactic forms before
   adjusting conversion ranks.

The concrete reproductions and C++ oracle outputs are in `scratch/rev1/`.

- (C6 round 3, 2026-09-29) Scalar-reference sets at `new` resolve through clang: `new Rf(d)` / `new Cc(d)` / `new Rr(d)` (const int&, const long&, int&&) with a double or float local are accepted with clang's value (master failed with an internal wrapper mismatch), and `Lr(int) + Lr(int&)` with a double local stays accepted like master, while `new A(d)` with only A(int) is refused. Part of the "float -> int in C++ ctor args" ruling item above.
- (C6 review 4) Intended new acceptances at `new` (clang values, expression form already accepted them on master): int -> float (`new Fs(i)`, Fd, Fl with an int or long local) and pointer -> bool (`new Bp(p)` with an int* local; bool is exempt from the pointer-is-not-a-number ruling).
