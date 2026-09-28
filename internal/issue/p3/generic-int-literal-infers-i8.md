# Generic type inference from an integer literal picks i8

`T first<T>(T a, T b)`: `first(300, 2)` is refused (300 does not fit the inferred i8) and
`sizeof(first(7, 2))` is 1. CFlat types literals narrowest, but inferring a generic T from a literal
should use the default integer type (int) as C++ does. Pre-existing on master 38cfb665; found
2026-09-27 by the fix/sizeof-eval round-3 review. Related open question: `sizeof(1) == 1`.
