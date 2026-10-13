# Local array with a ternary constant dimension: sizeof reports one element

Found by T96 review 1 (2026-10-10), pre-existing on master. `int[(2.0 > 1.0) ? 2 : 1] a = default;`
inside a function gives sizeof 4; clang (and the same declaration at global scope) gives 8.
`int[(1e400 > 1e308) + 1]` (no ternary) is correct. Repro: scratch/repro_keep/t96/rev_t96_arrays.cb
(prints "arrays 8 8 8 8 8" with clang).

Fix direction: the local declarator's constant-dimension evaluation does not fold a conditional
expression (falls back to 1?); share the global path's folder. Check whether the storage is also
one element (out-of-bounds writes) or only sizeof is wrong.
