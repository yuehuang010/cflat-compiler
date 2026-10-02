# CFlat `move` of a C++ class: sink shapes still byte-copy (N56 leftovers)

Ruling 2026-10-01: CFlat `move x` of a C++ class value mimics std::move. N48 + N56 cover init, auto, assignment (non-trivial), return, by-value arguments. Remaining, all master-identical (Opus review of N56, 2026-10-01; repros scratch/repro_keep/n56_rv2/):
1. Array init, array-element source (typed decl and by-value argument), ternary arms, brace field init, typed field source, vector push_back/emplace_back of an array element: bypass clang's pick (ctor template not run, source zeroed).
2. Deleted move accepted at array/brace/ternary sinks (clang refuses); explicit move ctor used at array/brace init (clang copies).
