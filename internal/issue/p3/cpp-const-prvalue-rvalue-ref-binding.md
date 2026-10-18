# A const C++ class prvalue binds to a non-const `R&&` parameter

Summary: when a C++ function returns `const R` by value, its result binds to an `R&&` parameter. Clang treats the result as a const
prvalue: it refuses `R&&` and picks a `const R&&` overload. T322 (2026-10-16) made the result a const receiver for member calls, but it
did not change argument binding. Found by the T322 round-1 review; the wrong behaviour is identical on master and the T322 branch.

Repro (header): `struct R { int v = 0; }; inline const R make() { return {}; }`
`inline int bind(R&& r) { return 11; } inline int bind(const R&& r) { return 12; } inline int onlyrv(R&& r) { return 13; }`
- `return rv322.bind(rv322.make());` -> CFlat 11, clang 12.
- `return rv322.onlyrv(rv322.make());` -> CFlat 13, clang refuses (binding `R&&` to a const value discards qualifiers).

Fix direction: overload ranking and reference-binding admissibility should read the call-result top-level const that T322 carries
(the TypeAndValue marker transferred into IsTopLevelConst). A const prvalue must not bind to non-const `R&&` or `R&`. It can bind to
`const R&&` (preferred) and to `const R&`.
