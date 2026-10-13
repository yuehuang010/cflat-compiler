# Arithmetic result type names differ from C/C++ (after T133)

PRE-EXISTING (T133 review 1, 2026-10-12); visible through overload picks on the result
(`g<T>(expr)` / direct `id(expr)` overload sets).

1. A `+`/`-` step after an operator-overload step drops `longdouble`: native
   `double operator+(SV, SV)`, `g(a + b + ld)` picks double (clang long double). Cause:
   MainListener_Expressions.cpp ~12400 `leftNativeIdentity` falls back to InferTernaryArmType(lv)
   on the ORIGINAL left operand; the overload branch clears lhsNativeArithmeticTypeName (~12577).
   Seed it from the operator's scalar return name instead. Repro scratch/repro_keep/t133/rev_t133_opchain.cb.
2. A native or C++ operator returning long double: `g(x + y)` picks double (clang long double).
3. Integer result names: `l + i`, `u + l`, `i & l` -> i64 (clang long); `ul + l` -> u64 (clang
   unsigned long); `i << l` -> i64 (clang int: shift result is the promoted LEFT type); `q - p` -> i64
   (clang ptrdiff_t = long); `s + s` stays short (clang int, integer promotion); `c ? c : c` -> int
   (clang char).
Item 3 needs a decision on whether CFlat's fixed-width names (i64) should spell as C's `long` at
overload ranking, or only where they differ in rank (promotion of short/char is a real rule).
