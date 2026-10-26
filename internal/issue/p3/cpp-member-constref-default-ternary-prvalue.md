# C++ `c ? default : local` into const T& is still not a prvalue for some call shapes

Fixed for: a static member or constructor taking `const T&` (declaration and expression forms), and a
constructor overload beside an int one (T459 r2/r3). The instance-member form already matched clang.
These five shapes still take the local arm as an lvalue. Master and this branch give the same values.

The fixture is `Test/library/cpp_interop_ternaryref.h` (`ternref::T391Val`, which counts ctor, copy,
move, dtor and live allocations, packed as value * 100000 + counts). Each value below is `c=false` /
`c=true`; clang++ -std=c++20 is the oracle. Repros: `scratch/repro_keep/box1022/t459/`
(each `.cb` is a CFlat repro, each `.cpp` its clang oracle; `fixture.h` and the header sit beside them).
Run from that directory: `x64/Release/cflat <name>.cb -o out.exe`, and `clang++ -std=c++20 <name>.cpp`.

## Shapes

1. Static member overload set. `Host::over(const V&)` beside `Host::over(int)`, `Host::over(c ? default : v)`,
   v a local `V`. master/fix 6710010 / 6020020, clang 6711020 / 6020020. Repro `over_static.cb`.
   The static deferral (MainListener_PostfixExpression.cpp, `cxxStaticMemberWrapper`) requires a bound
   symbol with an exact parameter count, so an overload set is not deferred.

2. Static member with an omitted default argument. `Host::def(const V&, int n = 9)` called with one argument.
   master/fix 6610010 / 5920020, clang 6611020 / 5920020. Passing `9` explicitly is fixed. Repro `def_static.cb`.
   Same gate: the parameter count does not match the argument count.

3. Static member template. `Host::temp<T>(const V&, T n)` called `temp(c ? default : v, 9)`.
   master/fix 6610010 / 5920020, clang 6611020 / 5920020. Repro `template_static.cb`.
   The template path does not take the deferred shape.

4. Brace initialization of a constructor taking const T&. `rev.Sink s{c ? default : v};`
   master/fix 710010 / 20011, clang 711020 / 20020. Repro `brace.cb`.
   The brace-list lowering in MainListener_Declarations.cpp (`ctorArgs` loop, the brace branch) and
   MainListener_Expressions.cpp do not arm the const T& temporary destination.

5. `new` of a constructor taking const T&. `rev.Sink* s = new rev.Sink(c ? default : v);`
   master/fix 710010 / 20011, clang 711020 / 20020. Repro `new.cb`.
   The new-expression path does not arm the destination either.

## Fix direction

- Shapes 1-3: let the deferred shape pass through static overload selection and omitted default
  arguments, instead of requiring a bound symbol with an exact parameter count. Check the
  overload actually selected, as the constructor path does (its place-type gate, see
  `IsCxxDeferrableDefaultTernary`).
- Shapes 4-5: apply the constructor argument lowering (`ParseCxxConstRefTernaryTemp` in
  MainListener_Expressions.cpp) to the brace-list and new-expression paths.

## Arm class differs from the const-ref parameter class (T459 Sol review 2, same on master)

T459 arms the conditional prvalue only when the `?:` place arm is the parameter's own class (so `Mixed(int)`
overload selection is kept). An arm whose class derives from, or converts to, the parameter class still misses
the conditional's copy:

- Derived arm: declaration master/fix `710010 20011` (true arm leaves one allocation live), clang `711020 20020`;
  expression master/fix `710010 20020`, clang `711020 20020`.
- Converting-ctor arm: declaration and expression master/fix `110700100 110300100`, clang `110701100 110300100`
  (the false arm omits the source-class copy before conversion).

Repros + clang oracles: scratch/repro_keep/box1022/t459/sol_r2/ (derived_*, convert_*; delta_fixture.h, fixture.h).
Fix direction: at MainListener_Declarations.cpp ~6123 and MainListener_PostfixExpression.cpp ~9174, materialize
the conditional's own class first, then apply the parameter conversion; do not just drop the class-equality check
(that reopens the `Mixed(int)` regression).
