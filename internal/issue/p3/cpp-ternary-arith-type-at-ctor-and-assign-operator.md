# Ternary of int literals keeps a CFlat width at a C++ ctor call and at operator=(T)

Pre-existing on master; found by the T55 review (2026-10-06). T55 (fix/t55-ternarylong) types a scalar
`?:` with C++ usual arithmetic conversions only where the argument is marked a C++ call argument
(PostfixExpression ~7034). Two other positions still give the CFlat width: `rv.C(k ? 5 : 6)` (ctor
template deduces signed char, clang int) and `c = k ? 5 : 6;` through a C++ `template<class T>
operator=(T)` (signed char, clang int). `rv.C d = k ? 5 : 6;` is refused on master and T55.
Repros: scratch/repro_keep/t55_rev/. Run AFTER T55 lands (same predicate, extend its marking to the
ctor-argument and assignment-operator paths).
