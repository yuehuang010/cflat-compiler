// M98: an ARRAY of a C++ class must default-CONSTRUCT every element. The array declaration paths
// used to store a whole-array zeroinitializer while scope exit already ran one destructor per
// element, so the two disagreed: the constructor's invariant was missing, the destructor ran on
// zero bytes, and a polymorphic element had a null vptr.
#pragma once

namespace cppa {

inline int& ctor_slot() { static int c = 0; return c; }
inline int& dtor_slot() { static int d = 0; return d; }
inline void reset() { ctor_slot() = 0; dtor_slot() = 0; }
inline int ctor_count() { return ctor_slot(); }
inline int dtor_count() { return dtor_slot(); }

// Nontrivial default constructor AND destructor - the construct/destroy asymmetry.
struct Trk { int v; Trk() : v(7) { ++ctor_slot(); } ~Trk() { ++dtor_slot(); } };

// Nontrivial default constructor, TRIVIAL destructor: nothing is destroyed, so the asymmetry is
// invisible, but the constructor's invariant is still missing without a per-element call.
struct CtorOnly { int v; CtorOnly() : v(9) { ++ctor_slot(); } };

// Fully trivial - the whole-array zeroinitializer stays correct for this one (accept set).
struct Plain { int v; };

// Polymorphic: the constructor writes the vptr, so a virtual call on an element used to crash.
struct Poly {
    int v;
    Poly() : v(5) { ++ctor_slot(); }
    virtual int kind() const { return 11; }
    virtual ~Poly() { ++dtor_slot(); }
};

// Default constructor DELETED: an array of it has no default initialization in C++ either.
struct NoDef { int v; NoDef() = delete; NoDef(int a) : v(a) {} ~NoDef() {} };

}
