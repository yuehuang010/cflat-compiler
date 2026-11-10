# C++ `c ? default : local` into const T& is still not a prvalue for static member calls

Fixed for: instance members and constructors taking `const T&` (declaration, expression, brace-init,
and `new` forms), a constructor overload beside an int one (T459 r2/r3), static member overload
sets, and static member calls with an omitted default argument. The static member template shape
remains open after three failed implementation attempts; do not retry it in this run.

The fixture is `Test/library/cpp_interop_ternaryref.h` (`ternref::T391Val`, which counts ctor, copy,
move, dtor and live allocations, packed as value * 100000 + counts). Repros: `scratch/repro_keep/box1022/t459/`
(each `.cb` is a CFlat repro, each `.cpp` its clang oracle; `fixture.h` and the header sit beside them).

## Remaining shape

Static member template. `Host::temp<T>(const V&, T n)` called as `temp(c ? default : v, 9)`.
Master/worktree before fix: `6610010 / 5920020`; clang++ -std=c++20: `6611020 / 5920020`.
The template path still does not carry the deferred shape through specialization selection. Repro:
`template_static.cb`.

## Remaining fix direction

Allow a static member template candidate through only when its selected viable parameter is a const
reference to the ternary place class; pass the deferred ternary into the template request so clang
performs deduction and call lowering. Preserve the int-overload control in `over_static.cb`.

## Arm class differs from the const-ref parameter class (T459 Sol review 2, same on master)

T459 arms the conditional prvalue only when the `?:` place arm is the parameter's own class (so `Mixed(int)`
overload selection is kept). An arm whose class derives from, or converts to, the parameter class still misses
the conditional's copy:

- Derived arm: declaration master/fix `710010 20011` (true arm leaves one allocation live), clang `711020 20020`;
  expression master/fix `710010 20020`, clang `711020 20020`.
- Converting-ctor arm: declaration and expression master/fix `110700100 110300100`, clang `110701100 110300100`
  (the false arm omits the source-class copy before conversion).

Repros + clang oracles: `scratch/repro_keep/box1022/t459/sol_r2/` (derived_*, convert_*; delta_fixture.h, fixture.h).
Fix direction: at MainListener_Declarations.cpp ~6123 and MainListener_PostfixExpression.cpp ~9174, materialize
the conditional's own class first, then apply the parameter conversion; do not just drop the class-equality check
(that reopens the `Mixed(int)` regression).
