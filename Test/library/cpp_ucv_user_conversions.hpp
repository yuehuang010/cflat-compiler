#pragma once

// User-defined conversions whose result is a pointer: const pointee, base pointee at a non-zero
// base offset, and identity-vs-qualification ranking at a C++ pointer parameter.
namespace ucv {
struct Oth { int z = 9; };
struct P2 { int v = 3; };
struct D2 : Oth, P2 {};

inline int takeBase(const P2* p) { return p->v; }
inline int pickMut(P2* p) { return 1; }
inline int pickMut(const P2* p) { return 2; }

// Result is a const base pointer: the mutable parameter is not viable, the const one is.
struct ConstBaseSource { D2* d = nullptr; operator const D2*() const { return d; } };
// Result is a mutable pointer: identity beats the qualification-adding const overload.
struct MutableSource { P2* m = nullptr; operator P2*() const { return m; } };
// Result is a const pointer: only the const overload is viable.
struct ConstSource { const P2* c = nullptr; operator const P2*() const { return c; } };
// Result is a const pointer: the mutable-only parameter must refuse it.
inline int takeMutable(P2* p) { return 1; }

// Explicit P2* conversion plus implicit const P2*: only the implicit const operator is usable.
struct ExplSource { P2* m = nullptr; explicit operator P2*() const { return nullptr; } operator const P2*() const { return m; } };
// Two derived pointer results: the base-most class (D2) is the one the call takes.
struct E2 : D2 {};
struct DerivedPick { E2* e = nullptr; D2* d = nullptr; operator E2*() const { return e; } operator D2*() const { return d; } };
inline int takeP2(P2* p) { return p->v; }
// Mutable int* result: identity beats the const int* overload.
struct IntSource { int* i = nullptr; operator int*() const { return i; } };
inline int pickInt(int* p) { return 1; }
inline int pickInt(const int* p) { return 2; }
// Derived mutable result for a CFlat callee taking P2*: must be refused (no CFlat-side pick).
struct D2Source { D2* d = nullptr; operator D2*() const { return d; } };
// Two unrelated derived results (neither base of the other): ambiguous, refused.
struct UnrelA : P2 {};
struct UnrelB : P2 {};
struct UnrelSource { UnrelA* a = nullptr; UnrelB* b = nullptr; operator UnrelA*() const { return a; } operator UnrelB*() const { return b; } };
}
