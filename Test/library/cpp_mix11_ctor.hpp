#pragma once
// MIX11: a by-value and a const& constructor parameter tie for an lvalue V2 (clang: ambiguous).
struct Mx11V2 { int x; Mx11V2(int n) : x(n) {} };
struct Mx11Amb
{
    int r;
    Mx11Amb(const Mx11V2& v) : r(v.x + 10) {}
    Mx11Amb(Mx11V2 v) : r(v.x + 20) {}
};

// CVAMB: free-function and method overloads of a reference and a by-value parameter (clang oracle).
// Pairs that clang ranks tie-free return distinct codes; the tied pairs are refusals in
// Test/errors/err_cpp_constref_byvalue_ambiguous.cb.
struct CvaV { int x; CvaV(int n) : x(n) {} };
struct CvaD : CvaV { CvaD(int n) : CvaV(n) {} };
inline int cva_a(const CvaV&) { return 1; }
inline int cva_a(CvaV) { return 2; }
inline int cva_b(CvaV&) { return 1; }
inline int cva_b(CvaV) { return 2; }
inline int cva_c(CvaV&&) { return 1; }
inline int cva_c(CvaV) { return 2; }
inline int cva_d(const CvaV&) { return 1; }
inline int cva_d(CvaV&&) { return 2; }
inline int cva_e(const CvaV&, int) { return 1; }
inline int cva_e(CvaV, long) { return 2; }
inline int cva_f(CvaV, int) { return 2; }
inline int cva_f(const CvaV&, long) { return 1; }
inline int cva_g(const CvaV&) { return 1; }
template<class T> inline int cva_g(T) { return 3; }
inline int cva_h(CvaV) { return 2; }
template<class T> inline int cva_h(const T&) { return 3; }
inline int operator+(const CvaV&, int) { return 1; }
inline int operator+(CvaV, int) { return 2; }
struct CvaCtr { int r; CvaCtr(const CvaV&) : r(1) {} CvaCtr(CvaV) : r(2) {} };
struct CvaM
{
    int m;
    CvaM(int n) : m(n) {}
    int mb(CvaV&) { return 1; }
    int mb(CvaV) { return 2; }
};
inline int cva_fi(const CvaV&) { return 1; }
inline int cva_fi(CvaV) { return 2; }
// CVAMB r3: a diamond (two non-virtual paths to CvaV) and a base with a deleted copy ctor, into a lone by-value parameter.
struct CvaDA : CvaV { CvaDA(int n) : CvaV(n) {} };
struct CvaDB : CvaV { CvaDB(int n) : CvaV(n) {} };
struct CvaDM : CvaDA, CvaDB { CvaDM(int n) : CvaDA(n), CvaDB(n) {} };
struct CvaCL { int r; CvaCL(CvaV) : r(2) {} };
struct CvaNB { int x; CvaNB(int n) : x(n) {} CvaNB(const CvaNB&) = delete; };
struct CvaND : CvaNB { CvaND(int n) : CvaNB(n) {} };
struct CvaCN { int r; CvaCN(CvaNB) : r(2) {} };
struct CvaCN2 { int r; CvaCN2(const CvaNB&) : r(1) {} CvaCN2(CvaNB) : r(2) {} };
