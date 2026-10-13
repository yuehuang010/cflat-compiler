# An array field of a struct with only a non-default constructor is accepted and never constructed

Summary: `struct HN { T m; int k = 1; HN(int x) { k = x; } }; struct O { HN[2] h; };` - `O o = { h =
{ { m = 5 }, { m = 6 } } };` compiles; no HN constructor runs (k stays 1) and the overrides build m
fresh (2/2 counts). clang++ refuses the equivalent (no default constructor for the array elements).
Master and T129 alike.

Found: T129 review 1 (2026-10-11), pre-existing. Needs a maintainer ruling on the surface: refuse
like C++ ("no default constructor for element type 'HN'"), or define CFlat semantics for an
element type whose only constructor takes arguments.
