# `new VB()` of a C++ class with a virtual base fails to compile

Found 2026-10-15 by the T247 review (pre-existing on master 68f3b221e). Repro
scratch/repro_keep/t247_rev/cases/e_vb.cb: `new VB()` where `struct VB : virtual A {...}` is refused
"cannot compute size of unsized or unresolved type"; clang compiles it. The record layout for a class with a
virtual base is not materialized on the `new` path. Check a local `VB v;` and `new VB[2]` alongside.
