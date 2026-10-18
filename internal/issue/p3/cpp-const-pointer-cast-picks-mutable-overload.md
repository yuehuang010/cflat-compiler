# Explicit `(const char*)p` cast picks the mutable C++ overload

Summary: with C++ overloads `pick(char*)` and `pick(const char*)`, CFlat `probe.pick((const char*)p)` (p a
`char*`) selects `pick(char*)` (22); clang selects `pick(const char*)` (11). Pre-existing on master d3681293f,
unchanged by const phase 0 (c00 review, 2026-10-17). Iteration 1 of the const plan is native-only; this is the
C++ interop side (internal/plan/cflat-const.md phase 5).

Repro: scratch/repro_keep/const_c00/rev_c00_sol_constcast.cb + .h (+ .cpp clang oracle) -> exit 11 (22-11).

Fix direction: the cast result should carry pointee const into C++ overload ranking. After phase 0 the cast
type has ConstMask (whole-word `const char*` = 0b11); CxxPointeeConstRelation (LLVMBackend.h) ranks only
IsCxxPointeeConst / IsSpelledPointeeConst. Fold the cast's ConstMask in when phase 5 unifies the flags.
