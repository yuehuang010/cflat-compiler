# Native brace initializer does not refuse narrowing constants

Summary: an array or aggregate brace initializer of native CFlat type accepts constants that do not
fit the element type. clang++ refuses each of these as a narrowing conversion ([dcl.init.list]).
Found while fixing the hex literal ladder (T76, 2026-10-09). The C++ brace path (braced
initializer_list / constructor arguments of imported C++ classes) already refuses them.

Repro (master 2026-10-09, Release, compiles clean):
    int[1] c = {3000000000};   // clang: narrowing
    u8[1] b = {300};           // clang: narrowing
    int[1] a = {0xFFFFFFFF};   // clang: narrowing (constant 4294967295 to int)
    int[1] n = {-1};           // ok in clang (fits)

Fix direction: a narrowing check on native brace elements, constants only (a non-constant element
keeps CFlat's existing rules). Decide the non-constant case with the maintainer before building it.
Not hex-specific: the same gap exists for decimal and small types, so it is not part of the ladder fix.
