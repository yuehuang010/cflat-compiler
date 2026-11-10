# CFlat `move` of a C++ class: sink shapes still byte-copy (N56 leftovers)

Ruling 2026-10-01: CFlat `move x` of a C++ class value mimics std::move. N48 + N56 cover init, auto, assignment (non-trivial), return, by-value arguments. The 2026-11-07 sinkshape pass resolves the simple fixed-array initializer and deleted-move/explicit-move fixed-array cases; remaining probes still reproduce against clang++ -std=c++20:
1. Array-element source into a typed local and by-value argument, ternary arms, field-source move into auto/typed local, brace field initialization, and vector push_back of an array element: CFlat still bypasses or repeats clang's selected constructor.
2. Deleted move is still accepted at brace and ternary sinks; explicit move constructor is still used at brace field initialization where clang copies.

Repros: `scratch/repro_keep/n56_rv2/old/` and `scratch/repro_keep/n56_rv2/p/` in the main checkout. This issue remains active.
