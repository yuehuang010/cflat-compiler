# long double identity lost in native generic deduction and in a ternary

Found by the T100 review (2026-10-10), pre-existing on master.

## Repro
Native overloads `int p3(longdouble)=3`, `int p3(double)=2`, `int p3(float)=1`:
- `pick<T>(T x) { return p3(x); }` with `ld + 1.0f`, `ld + d`, `d + 0.25l`: cflat 2, clang 3.
  `0.25l * 2`: cflat 2, clang 3. A direct `p3(ld + 1.0f)` is 3, so direct and generic calls disagree.
  Literals, variables, `arr[0]`, `*pld` and `auto` deduce longdouble correctly.
- `p3(c ? 0.25l : 1.0f)`: cflat 2, clang 3. `p3(c ? ld : 1.0)`: cflat 2, clang 3.
Repros: scratch/repro_keep/t100/c_nat_T_{11,12,13,14}.cb, scratch/repro_keep/t100/x_tern.cb.

## Root cause (direction)
double and long double share the LLVM type, so the long double identity rides on the TypeAndValue
TypeName. Generic T deduction from an arithmetic result and the ternary result type rebuild the type from
the LLVM value (double) instead of carrying the usual-arithmetic-conversion result name `longdouble`.

## Fix direction
Carry `longdouble` through binary arithmetic results into generic deduction, and compute the ternary
result type with the usual arithmetic conversions on the arm type NAMES.
Acceptance: all five shapes pick 3 like clang; existing test_basic long double legs unchanged.
