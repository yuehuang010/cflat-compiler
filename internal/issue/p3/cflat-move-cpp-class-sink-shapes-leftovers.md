# CFlat `move` of a C++ class: sink shapes still byte-copy (N56 leftovers)

Ruling 2026-10-01: CFlat `move x` of a C++ class value mimics std::move. N48 + N56 cover init, auto, assignment (non-trivial), return, by-value arguments. The 2026-11-07 sinkshape pass resolves the simple fixed-array initializer and deleted-move/explicit-move fixed-array cases; remaining probes still reproduce against clang++ -std=c++20:
1. Ternary arms: `auto c = cond ? move a : move b` and arms reading an element still byte-copy (or leave a zeroed source) for a trivially copyable class with a constructor template; a deleted move at a ternary sink is still accepted.
2. `[cpp] struct Box { edge.Triv item; }; Box b = move a` (class-level move of a CFlat `[cpp]` struct whose field needs a selected constructor) still byte-copies the field: value 7, clang 107/m1.
3. A `T(args)` prvalue passed through the foreign aggregate brace wrapper (`edge.Holder b = {edge.Triv(7)}`) runs the constructor template once more than clang's elided form: value 107 and m1, clang 7/m0.

Repros: `scratch/repro_keep/n56_rv2/old/` and `scratch/repro_keep/n56_rv2/p/` in the main checkout. This issue remains active.
