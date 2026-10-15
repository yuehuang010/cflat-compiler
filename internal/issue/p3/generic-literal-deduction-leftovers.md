# Generic T deduction from literals: leftovers after T152

T152 (2026-10-13) makes a plain `T` parameter fed an unsuffixed literal take the literal's C++ default type. Sol T152 review 1
found these, all master-identical (pre-existing). Probes: scratch/repro_keep/t152/ (rev_t152_sol_probe.sh + sources).

1. Ternary of literals and `-(7)` still infer the narrowed 1-byte type (`sizeof(first(c ? 1 : 2, 3))` 1, clang 4).
2. (BY DESIGN, removed 2026-10-13) `f(i8)` next to `f<T>(T)` called with `7` picks the non-generic i8 overload: non-generic
   overload resolution sees the narrowest literal type (doc/LANGUAGE.md).
3. Native constructor / brace type deduction and inferred instance generic methods are unavailable; clang accepts the oracle forms.
4. `list<T>` parameter deduction from a typed list is unavailable; clang accepts.

RULED 2026-10-13 (maintainer, no preference -> recommended option kept): T152's rule stays and is documented in
doc/LANGUAGE.md (generic deduction is the exception to narrowest literal typing). Former open ruling: two literals that need different types widen (`first(5000000000, 2)` -> long,
`first(1.5, 2.5f)` -> double) where C++ rejects the deduction conflict. Keep widening or refuse like C++?
