# Arithmetic result type names differ from C/C++ (after T133)

PRE-EXISTING (T133 review 1, 2026-10-12); visible through overload picks on the result
(`g<T>(expr)` / direct `id(expr)` overload sets).

1. A `+`/`-` step after an operator-overload step drops `longdouble`: native
   `double operator+(SV, SV)`, `g(a + b + ld)` picks double (clang long double). Cause:
   MainListener_Expressions.cpp ~12400 `leftNativeIdentity` falls back to InferTernaryArmType(lv)
   on the ORIGINAL left operand; the overload branch clears lhsNativeArithmeticTypeName (~12577).
   Seed it from the operator's scalar return name instead. Repro scratch/repro_keep/t133/rev_t133_opchain.cb.
2. A native or C++ operator returning long double: `g(x + y)` picks double (clang long double).
3. Fixed in ARITH (2026-10-31): integer result names and widths follow C's usual arithmetic conversions
   (promotion, shift = promoted left, ternary same-type, ptrdiff); matrix of 917 cells matches clang.

RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31), item 3 in part: the NAME is not a ranking matter - C
spellings are aliases of the fixed-width types (ruling 2026-09-10), so `i64` and LP64 `long` are one identity and rank the same;
`long` operands follow the target `long` (L-suffix ruling 2026-09-30), so an LLP64 `l + i` must be 32-bit, not i64. Diagnostics
print the user's spelling where the expression carries one, else the fixed-width name. The PROMOTION sub-question (ruled below, 2026-10-31) - whether `s + s`, `c ? c : c`, `i << l`, `q - p` follow C's usual arithmetic conversions (int, char, int,
ptrdiff_t) or keep CFlat's narrowest typing; that changes overload picks of compiling programs.
RULED 2026-10-31 (maintainer): FOLLOW C's usual arithmetic conversions for the result type of a native expression - `short`/`char`
operands promote to `int` (`s + s` is int), the shift result is the promoted LEFT operand type (`i << l` is int), a same-type
ternary keeps the type (`c ? c : c` is char), pointer difference is `ptrdiff_t` (target long). C, C++ and C# agree; CFlat has no
safety reason to differ. Assignment narrowing stays accepted as today (`short r = s + s;` compiles, probe scratch/rulings_1031/narrow.cb),
so the change is visible only through overload picks and C++ template deduction on such results. Test consequence: pins of the
narrow result type in test_generics / test_cpp_interop flip to clang's picks; items 1-2 (longdouble drops) are bugs, unchanged.
