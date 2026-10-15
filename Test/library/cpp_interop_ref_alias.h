#pragma once

#include <utility>

/*
 * M98 - C++ REFERENCE data members as ALIASES of the referent: a read loads it, a store writes
 * through it, `&obj.r` is its address. Reseating is impossible (no pointer slot is ever assigned).
 */
namespace cppalias {

struct Holder {
    int& r;
    const int& c;
    Holder(int& x, const int& y) : r(x), c(y) {}
};

struct Inner {
    Holder h;
    explicit Inner(Holder x) : h(x) {}
};

struct Outer {
    Inner inner;
    explicit Outer(Inner i) : inner(i) {}
};

inline std::pair<const int&, const int&> both(int& a, int& b) { return {a, b}; }

inline int bump_ref(int& v) { return ++v; }
// Call results that are references to scalars: postfix ++/-- acts on the returned referent.
inline int& pick_int(int& v) { return v; }
inline short& pick_short(short& v) { return v; }
inline char& pick_char(char& v) { return v; }
inline double& pick_double(double& v) { return v; }
inline int read_const(const int& v) { return v; }


// Pointer referents (round 2): globals bound through factories that take no arguments.
struct PtrRef { int*& p; };
struct PtrPtrRef { int**& pp; };
struct CPtrRef { const int*& p; };
struct CPPRef { int* const& p; };
struct CtorPtrRef {
    int*& p;
    explicit CtorPtrRef(int*& x) : p(x) {}
};
struct CtorPtrRefList {
    int*& p;
    CtorPtrRefList(int*& x) : p(x) {}
};
struct CtorCPtrRef {
    const int*& p;
    explicit CtorCPtrRef(const int*& x) : p(x) {}
};
struct CtorPtrConstRef {
    int* const& p;
    explicit CtorPtrConstRef(int* const& x) : p(x) {}
    bool aliases(int* const& x) const { return &x == &p; }
};
struct CtorPtrRRefTemplate {
    int seen;
    template<class Dummy = void>
    explicit CtorPtrRRefTemplate(int*&& x) : seen(*x) {}
};
inline void t131_clobber_stack() {
    volatile int fill[256] = {};
    for (int i = 0; i < 256; ++i) fill[i] = i;
}
struct Vv { int n; int get() const { return n; } void set(int x) { n = x; } };
struct VPRef { Vv*& v; };
inline int pw = 6;
inline int pv = 4;
inline int* pgp = &pw;
inline int** ppg = &pgp;
inline const int* cpgp = &pw;
inline Vv cv = {7};
inline Vv* cvp = &cv;
inline PtrRef global_pref() { return PtrRef{pgp}; }
inline PtrPtrRef global_ppref() { return PtrPtrRef{ppg}; }
inline CPtrRef global_cpref() { return CPtrRef{cpgp}; }
inline CPPRef global_cpp() { return CPPRef{pgp}; }
inline VPRef global_vpref() { return VPRef{cvp}; }
inline void set_pw(int v) { pw = v; }
inline void reset_pointers() { pw = 6; pgp = &pw; cpgp = &pw; pv = 4; }
inline int pw_value() { return pw; }


// Round 3: class-template referent with a pointer argument; mutable and const-pointer shapes.
template <class T> struct Box { T p; int get() const { return *p; } void set(int n) { *p = n; } };
template struct Box<int*>;
struct BoxRef { Box<int*>& b; };
struct MutM { mutable int m; int x; };
struct MutRef { const MutM& r; };
struct Vv2 { int n; int get() const { return n; } void set(int x) { n = x; } };
struct VPCRef { Vv2* const& v; };
struct CVPCRef { const Vv2* const& v; };
inline int gn = 6;
inline Box<int*> gbox = {&gn};
inline BoxRef global_boxref() { return BoxRef{gbox}; }
inline int gn_value() { return gn; }
inline MutM gmut = {3, 4};
inline MutRef global_mutref() { return MutRef{gmut}; }
inline int gmut_m() { return gmut.m; }
inline Vv2 gv2 = {7};
inline Vv2* gv2p = &gv2;
inline VPCRef global_vpc() { return VPCRef{gv2p}; }
inline CVPCRef global_cvpc() { return CVPCRef{gv2p}; }
inline int gv2_n() { return gv2.n; }
}
