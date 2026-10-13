# Plain expression elements written over a user-ctor array element leak the ctor-built field

Summary: `struct HU { T m; int k = 1; HU() { k = 2; } }; struct O { HU[2] h; };` with T a counted
C++ class. `HU x = default; O o = { h = { x, x } };` gives 5 constructions / 3 destructions; the
mixed `{ { m = 5 }, x }` gives 7/5 (master and T129 alike). HU() already built each element's m; the
element copy writes over the live element without destroying it.

Found: T129 review 1 (2026-10-11), pre-existing. T129 fixed the all-braced spelling
(`{ { m = 5 }, { m = 6 } }`); this is the expression / mixed spelling its all-braced check leaves on
the old path (MainListener_Expressions.cpp ~19920).

Fix direction: for a constructed user-ctor element, an expression element is an assignment into a
live object: destroy-then-copy (or call the element's assignment) like T129's braced route.
