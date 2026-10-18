# [cpp] override with an immovable by-value C++ class parameter is refused

Summary: a `[cpp] struct` overriding a virtual whose parameter is a C++ class with deleted copy AND move
constructors is refused; clang accepts the same override (the caller materializes the prvalue in place).
Found by the T331 round-1 review (2026-10-16), same behaviour on master before and after T331.

Repro (header):
`struct Immovable { int v; Immovable(int x):v(x){} Immovable(const Immovable&)=delete; Immovable(Immovable&&)=delete; };`
`struct BaseImmovable { virtual ~BaseImmovable()=default; virtual int use(Immovable x)=0; };`
`inline int callImmovable(BaseImmovable* b) { return b->use(Immovable{7}); }`
CFlat: `[cpp] struct Derived : rev.BaseImmovable { override int use(rev.Immovable x) { return x.v; } };`
-> `'__cflat_user::Derived' does not name a C++ class type in the imported headers`; clang twin returns 7.

Root cause (suspected): the generated C++ override wrapper forwards the by-value parameter into the CFlat body
by value (T331: as `T&&`, or `const T&` for copy-only types); with neither ctor available the twin fails to
compile and the class is dropped, surfacing as the unrelated "does not name a C++ class type" text.
Fix direction: pass an immovable by-value parameter to the CFlat body by reference (the body sees the wrapper's
parameter object), or refuse with a specific diagnostic naming the deleted constructors.
