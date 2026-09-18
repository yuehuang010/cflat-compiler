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

/*
 * M103: FILE-SCOPE globals of a C++ class. These use their OWN counters, never reset(), because
 * a global is constructed before main runs and every reset() in the fixture would erase the
 * evidence. RULING: the constructor runs before main and there is NO exit-time destruction, so
 * gdtor_count() must still read 0 at the end of main.
 */
inline int& gctor_slot() { static int c = 0; return c; }
inline int& gdtor_slot() { static int d = 0; return d; }
inline int gctor_count() { return gctor_slot(); }
inline int gdtor_count() { return gdtor_slot(); }
struct GTrk { int v; GTrk() : v(7) { ++gctor_slot(); } ~GTrk() { ++gdtor_slot(); } };
// Polymorphic: the constructor writes the vptr, so a virtual call on a zeroed global crashes.
struct GPoly {
    int v;
    GPoly() : v(5) { ++gctor_slot(); }
    virtual int kind() const { return 11; }
    virtual ~GPoly() { ++gdtor_slot(); }
};
// Declaration ORDER: each object records its own construction index on a private counter. Also
// carries the IMPORT order - an imported module's globals are constructed before its importer's.
inline int& gseq_slot() { static int c = 0; return c; }
struct GSeq { int v; GSeq() : v(++gseq_slot()) {} ~GSeq() { ++gdtor_slot(); } };

/*
 * ORDER against CLANG's own initializers: `g_reg` is a header-defined namespace-scope C++ static,
 * so Clang initializes it from its companion module's llvm.global_ctors entry. A cflat global
 * whose constructor READS it must therefore be constructed after it - which is only true if the
 * two lists are sequenced deliberately. Pre-fix the cflat entry was registered ahead of Clang's
 * at the same priority, and the Mach-O image and the ORC JIT walk that array in OPPOSITE
 * directions, so AOT saw 4242 and --run saw 0.
 */
struct GReg { int magic; GReg(); };
inline GReg::GReg() : magic(4242) {}
inline GReg g_reg;
struct GProbe { int saw; GProbe(); };
inline GProbe::GProbe() : saw(g_reg.magic) {}

// Default constructor DELETED: an array of it has no default initialization in C++ either.
struct NoDef { int v; NoDef() = delete; NoDef(int a) : v(a) {} ~NoDef() {} };

}
