# Floating literal leftovers: l / L suffix types float, decimal overflow folds to 0

From T86 (2026-10-10), pre-existing on master. `0.25l` and `0x1p-2l` are typed f32 in CFlat; C and C++ type
an l/L-suffixed floating literal as long double (CFlat `longdouble`; 64-bit on Apple arm64 and MSVC, 80-bit
x87 on x86-64 Linux). Visible through overload picks ({float, double, long double}) and C++ template
deduction (T deduces long double in clang).

## Fix direction
ParseNumberConstant (cflat/MainListener_Utilities.cpp) floating suffix rule: l/L -> longdouble, for decimal and
hex floats; check sizeof and an overload-pick oracle against clang++ -std=c++20.

## Also: decimal floating overflow folds to 0
`1e400` (and other decimal literals beyond double range) lowers to 0; clang gives inf with an overflow
warning. T86 fixed the hex-float path only (infinity kept in the suffix type); the decimal path in
ParseNumberConstant still drops it.
