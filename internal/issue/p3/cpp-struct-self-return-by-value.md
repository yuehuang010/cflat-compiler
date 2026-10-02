# [cpp] struct `return *this`-style self() by value diverges from clang

Found by Q1 Sol r1 (2026-10-01), pre-existing on master and branch. Repros: scratch/repro_keep/q1_followups/{cpppod.cb,cppstruct.cb} with clang oracles oracle_edges.cpp / oracle.cpp.

1. [cpp] plain-old-data struct: `self()` returning `*this` by value zeroes the source field (prints `0 17`); clang prints `17 17` (copy, source kept).
2. [cpp] struct `Holder` holding a copyable [cpp] `Twin`: `self()` refused with "copy constructor is deleted"; clang copies once and destroys twice.

Fix direction: a [cpp] struct is a C++ class - `return *this` by value is a copy via the (implicit) copy constructor, not a move-out. Check the returnIsThisDeref / lowered-return path for [cpp] receivers.
