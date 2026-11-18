#pragma once
#include <initializer_list>
// CVTIE: conversion-operator and braced-argument overload picks; refusals are in
// Test/errors/err_cpp_conversion_brace_ties.cb. Every expected value is the clang++ -std=c++20 answer.
struct CvtV { int v; CvtV(int n) : v(n) { } CvtV(const CvtV& o) : v(o.v) { } ~CvtV() { } };
struct CvtW { int v; CvtW(int n) : v(n) { } CvtW(const CvtW& o) : v(o.v) { } ~CvtW() { } };
struct CvtOne { int v = 1; operator CvtV() const { return CvtV(v); } };
struct CvtEx { int v = 1; operator CvtV() const { return CvtV(v); } explicit operator CvtW() const { return CvtW(v); } };
struct CvtBoth { int v = 1; operator CvtV() const { return CvtV(v); } operator CvtW() const { return CvtW(v); } };
struct CvtX { int v; CvtX(int n) : v(n) { } };
// One conversion is usable, so clang picks; the implicit-conversion wrappers must not tie with each other.
inline int cvt_bv(CvtV) { return 1; }
inline int cvt_bv(CvtW) { return 2; }
inline int cvt_one(CvtV) { return 1; }
inline int cvt_one(CvtX) { return 2; }
inline int cvt_sec(int, CvtV) { return 1; }
inline int cvt_sec(int, CvtW) { return 2; }
struct CvtM
{
    int m(CvtV) { return 1; }
    int m(CvtW) { return 2; }
    static int s(CvtV) { return 1; }
    static int s(CvtW) { return 2; }
};
// A tie across two conversion operators, broken only by another argument.
inline int cvt_tie(const CvtV&, long) { return 1; }
inline int cvt_tie(CvtV, int) { return 2; }
inline int cvt_tie(CvtW, int) { return 3; }
// Function pointers beside a braced list: `&` in a parameter list is no reference.
using CvtFp = void (*)(int&);
inline void cvt_cb(int&) { }
inline CvtFp cvt_getcb() { return cvt_cb; }
inline int cvt_fp(CvtFp, std::initializer_list<long>) { return 1; }
inline int cvt_fp(bool, std::initializer_list<long>) { return 2; }
inline int cvt_fps(bool, std::initializer_list<long>) { return 2; }
inline int cvt_fps(CvtFp, std::initializer_list<long>) { return 1; }
// `const` on a pointee or in a name is not a const reference.
inline int cvt_cp(const int*&, std::initializer_list<long>) { return 1; }
inline int cvt_cp(const void*, std::initializer_list<long>) { return 2; }
enum cvt_constant_e { CVT_KA = 0 };
inline int cvt_en(cvt_constant_e&, std::initializer_list<long>) { return 1; }
inline int cvt_en(int, std::initializer_list<long>) { return 2; }
// A non-const reference cannot bind a temporary: the candidate leaves the set, in either order.
inline int cvt_nv(int&, std::initializer_list<int>) { return 1; }
inline int cvt_nv(int, std::initializer_list<long>) { return 2; }
inline int cvt_nvs(int, std::initializer_list<long>) { return 2; }
inline int cvt_nvs(int&, std::initializer_list<int>) { return 1; }
struct CvtN
{
    int f(int&, std::initializer_list<int>) { return 1; }
    int f(int, std::initializer_list<long>) { return 2; }
};
// A pick that beats every rival is kept when a rival is only comparable to it.
inline int cvt_best(short, std::initializer_list<long>) { return 1; }
inline int cvt_best(short, std::initializer_list<int>) { return 3; }
// Non-transitive sets: no candidate beats every other one (refused).
inline int cvt_cross(short, std::initializer_list<long>) { return 1; }
inline int cvt_cross(long, std::initializer_list<short>) { return 2; }
inline int cvt_cross(short, std::initializer_list<int>) { return 3; }
struct CvtC
{
    int f(short, std::initializer_list<long>) { return 1; }
    int f(long, std::initializer_list<short>) { return 2; }
    int f(short, std::initializer_list<int>) { return 3; }
};
inline int cvt_nb(const CvtV&, int, long) { return 1; }
inline int cvt_nb(CvtV, short, long) { return 2; }
inline int cvt_nb(CvtV, long, int) { return 3; }
inline int cvt_nbok(const CvtV&, int, long) { return 1; }
inline int cvt_nbok(CvtV, short, long) { return 2; }
inline int cvt_nbok(CvtV, long, long) { return 3; }
// The implicit object ranks before the result's conversion, in either declaration order.
struct CvtP { int v = 0; };
struct CvtD : CvtP { CvtD() { v = 7; } };
struct CvtE : CvtD { CvtE() { v = 8; } };
inline CvtD cvtGd;
inline CvtE cvtGe;
inline CvtP cvtGp;
inline int cvt_p(CvtP* p) { return p->v; }
struct CvtS1 { operator CvtE*() { return &cvtGe; } operator CvtD*() const { return &cvtGd; } };
struct CvtS2 { operator CvtD*() const { return &cvtGd; } operator CvtE*() { return &cvtGe; } };
struct CvtS3 { operator CvtP*() const { cvtGp.v = 5; return &cvtGp; } operator CvtD*() { return &cvtGd; } };
// A non-const operator beats its const twin and a `const S&` constructor.
struct CvtTgtS;
struct CvtT { int v = 0; CvtT() { } CvtT(const CvtTgtS&) { v = 5; } };
struct CvtTgtS { operator CvtT() { CvtT t; t.v = 2; return t; } operator CvtT() const { CvtT t; t.v = 1; return t; } };
inline int cvt_t(CvtT t) { return t.v; }
inline int cvt_tr(const CvtT& t) { return t.v; }
// A global-namespace function template, deduced from a braced list.
template <class T> inline int cvt_gt(T v) { return (int)v + 2; }
template <class T> inline int cvt_gl(std::initializer_list<T> l) { return (int)l.size() + 40; }
// CVTIE r2: a wrapper bound for one value category or constness must not serve another call shape.
struct CvrQ { int v = 1; operator CvtV() & { return CvtV(1); } operator CvtW() && { return CvtW(9); } };
inline CvrQ cvr_mk() { return CvrQ(); }
inline int cvr_a(CvtV) { return 1; }
inline int cvr_a(CvtW) { return 2; }
inline int cvr_b(CvtV) { return 1; }
inline int cvr_b(CvtW) { return 2; }
struct CvrM { int m(CvtV) { return 1; } int m(CvtW) { return 2; } };
struct CvrNc { int v = 1; operator CvtV() { return CvtV(1); } };
inline const CvrNc cvrGnc{};
inline const CvrNc& cvr_getnc() { return cvrGnc; }
inline int cvr_c(CvtV) { return 1; }
inline int cvr_c(CvtW) { return 2; }
struct CvrMix { int v = 1; operator CvtV() const { return CvtV(1); } operator CvtW() { return CvtW(2); } };
inline const CvrMix cvrGmix{};
inline const CvrMix& cvr_getmix() { return cvrGmix; }
inline int cvr_d(CvtV) { return 1; }
inline int cvr_d(CvtW) { return 2; }
// CVTIE r2: from a pointer only bool is viable among the arithmetic parameters.
inline int cvr_bi(bool, std::initializer_list<long>) { return 2; }
inline int cvr_bi(int, std::initializer_list<long>) { return 1; }
inline int cvr_db(double, std::initializer_list<long>) { return 1; }
inline int cvr_db(bool, std::initializer_list<long>) { return 2; }
// CVTIE r2: a function-pointer value reaches bool or its own type, never another pointer type.
inline int cvr_fv(bool, std::initializer_list<long>) { return 1; }
inline int cvr_fv(void*, std::initializer_list<long>) { return 2; }
inline int cvr_fc(bool, std::initializer_list<long>) { return 1; }
inline int cvr_fc(const void*, std::initializer_list<long>) { return 2; }
inline int cvr_fo(bool, std::initializer_list<long>) { return 1; }
inline int cvr_fo(void (*)(long), std::initializer_list<long>) { return 2; }
inline int cvr_fown(void*, std::initializer_list<long>) { return 2; }
inline int cvr_fown(CvtFp, std::initializer_list<long>) { return 1; }
// CVTIE r2: a function reference is a reference, and a pointer value cannot bind it.
using CvrFr = void (&)(int&);
using CvrFt = void(int&);
inline int cvr_r1(void (&)(int&), std::initializer_list<long>) { return 1; }
inline int cvr_r1(bool, std::initializer_list<long>) { return 2; }
inline int cvr_r2(bool, std::initializer_list<long>) { return 2; }
inline int cvr_r2(void (&)(int&), std::initializer_list<long>) { return 1; }
inline int cvr_r3(CvrFr, std::initializer_list<long>) { return 1; }
inline int cvr_r3(bool, std::initializer_list<long>) { return 2; }
inline int cvr_r4(CvrFt&, std::initializer_list<long>) { return 1; }
inline int cvr_r4(bool, std::initializer_list<long>) { return 2; }
// CVTIE r2: a const source counts only const conversion operators.
struct CvrS;
struct CvrT { int v = 0; CvrT() { } CvrT(const CvrS&) { v = 5; } };
struct CvrS { operator CvrT() { CvrT t; t.v = 2; return t; } operator CvrT() const { CvrT t; t.v = 1; return t; } };
inline const CvrS cvrGs{};
inline const CvrS& cvr_gets() { return cvrGs; }
inline const CvrS* cvr_getsp() { return &cvrGs; }
inline int cvr_t(CvrT t) { return t.v; }
inline int cvr_tr(const CvrT& t) { return t.v; }
struct CvrS2;
struct CvrT2 { int v = 0; CvrT2() { } CvrT2(CvrS2&) { v = 5; } };
struct CvrS2 { operator CvrT2() { CvrT2 t; t.v = 2; return t; } operator CvrT2() const { CvrT2 t; t.v = 1; return t; } };
inline const CvrS2 cvrGs2{};
inline const CvrS2& cvr_gets2() { return cvrGs2; }
inline int cvr_t2(CvrT2 t) { return t.v; }
struct CvrT3 { int v = 0; CvrT3() { } };
struct CvrS3 { operator CvrT3() { CvrT3 t; t.v = 2; return t; } operator CvrT3() const { CvrT3 t; t.v = 1; return t; } };
inline const CvrS3 cvrGs3{};
inline const CvrS3& cvr_gets3() { return cvrGs3; }
inline int cvr_t3(CvrT3 t) { return t.v; }

// CVTIE r3: one predicate rules a pointer-shaped argument against each parameter.
enum CvsE { CVS_E0 }; enum class CvsSe { S0 };
using CvsFp = void (*)(int&);
using CvsFr = void (&)(int&);
using CvsVp = void (*)(void (*)(int));
inline void cvs_cb(int&) { }
inline CvsFp cvs_getcb() { return cvs_cb; }
inline const int* cvs_getcp() { static int g = 1; return &g; }
inline void (*cvs_getl())(long) { return nullptr; }
inline CvsVp cvs_getv() { return nullptr; }
#define CVS_IL std::initializer_list<long>
inline int cvs_a1(bool, CVS_IL) { return 1; } inline int cvs_a1(const int&, CVS_IL) { return 2; }
inline int cvs_a2(const int&, CVS_IL) { return 2; } inline int cvs_a2(bool, CVS_IL) { return 1; }
struct CvsM { int f(bool, CVS_IL) { return 1; } int f(const int&, CVS_IL) { return 2; } };
inline int cvs_b1(bool, CVS_IL) { return 1; } inline int cvs_b1(CvsE, CVS_IL) { return 2; }
inline int cvs_b2(bool, CVS_IL) { return 1; } inline int cvs_b2(wchar_t, CVS_IL) { return 2; }
inline int cvs_b3(bool, CVS_IL) { return 1; } inline int cvs_b3(CvsSe, CVS_IL) { return 2; }
inline int cvs_c1(CvsFr, CVS_IL) { return 1; } inline int cvs_c1(bool, CVS_IL) { return 2; }
inline int cvs_c2(bool, CVS_IL) { return 1; } inline int cvs_c2(void*, CVS_IL) { return 2; }
inline int cvs_d1(void (*)(const int&), CVS_IL) { return 1; } inline int cvs_d1(bool, CVS_IL) { return 2; }
inline int cvs_d2(void (*)(long long), CVS_IL) { return 1; } inline int cvs_d2(bool, CVS_IL) { return 2; }
struct CvsC { int k; CvsC(CvsE, CVS_IL) : k(2) { } CvsC(bool, CVS_IL) : k(1) { } };
inline int cvs_e2(void*, CVS_IL) { return 2; } inline int cvs_e2(bool, CVS_IL) { return 1; }
inline int cvs_u(CvsFp, CVS_IL) { return 2; } inline int cvs_u(bool, CVS_IL) { return 1; }
inline int cvs_n(void (*)(void (int)), CVS_IL) { return 1; } inline int cvs_n(bool, CVS_IL) { return 2; }
// A const class prvalue counts only its const operators.
struct CvsS; struct CvsT { int v = 0; CvsT() { } CvsT(const CvsS&) { v = 5; } };
struct CvsS { operator CvsT() { CvsT t; t.v = 2; return t; } operator CvsT() const { CvsT t; t.v = 1; return t; } };
inline const CvsS cvs_cmks() { return CvsS{}; }
inline int cvs_ct(CvsT t) { return t.v; }
inline int cvs_ctr(const CvsT& t) { return t.v; }

// CVTIE r4: unequal text is no proof of a different type.
inline void cvq_k(const int) { }
inline int cvq_a(void (*)(int), int) { return 2; } inline int cvq_a(bool, int) { return 1; }
inline void cvq_o(int) { } inline void cvq_o(long) { }
inline int cvq_b(void (*)(long), int) { return 2; } inline int cvq_b(bool, int) { return 1; }
using CvqR = void (*)(long); inline void cvq_l(long) { } inline CvqR cvq_g(int) { return cvq_l; } inline CvqR (*cvq_getg())(int) { return cvq_g; }
inline int cvq_c(CvqR (*)(int), int) { return 2; } inline int cvq_c(bool, int) { return 1; }
inline int cvq_d(void (*)(const int&), int) { return 2; } inline int cvq_d(bool, int) { return 1; }

// CVTIE r5: a named function is judged by its whole overload set and real type.
struct CvuN { static void g(int) { } static void g(long) { } };
inline int cvu_a(void (*)(long), int) { return 2; } inline int cvu_a(bool, int) { return 1; }
inline void cvu_t(const int, const int&) { }
inline int cvu_b(void (*)(int, const int&), int) { return 2; } inline int cvu_b(bool, int) { return 1; }
inline void cvu_o(int) { } inline void cvu_o(const int&) { }
inline int cvu_c(void (*)(const int&), int) { return 2; } inline int cvu_c(bool, int) { return 1; }
inline void cvu_n(const int&) noexcept { }
inline int cvu_d(void (*)(const int&) noexcept, int) { return 2; } inline int cvu_d(bool, int) { return 1; }
using CvuR = void (*)(long); inline CvuR cvu_r(int) { return nullptr; }
inline int cvu_e(CvuR (*)(int), CVS_IL) { return 2; } inline int cvu_e(bool, CVS_IL) { return 1; }
inline int cvu_f(CvuR (&)(int), CVS_IL) { return 1; } inline int cvu_f(bool, CVS_IL) { return 2; }
using CvuP = CvuR (*)(int); inline CvuP cvu_getr() { return cvu_r; }

// CVTIE r6: throw() is noexcept; a name with a template overload has no complete set.
inline void cvw_t(const int&) throw() { }
inline int cvw_a(void (*)(const int&) noexcept, int) { return 2; } inline int cvw_a(bool, int) { return 1; }
template <class T> void cvw_g(T) { } inline void cvw_g(int) { }
inline int cvw_b(void (*)(long), CVS_IL) { return 2; } inline int cvw_b(bool, CVS_IL) { return 1; }
inline void cvw_k(void (* const)(int)) { }
inline int cvw_c(void (*)(void (*)(int)), CVS_IL) { return 2; } inline int cvw_c(bool, CVS_IL) { return 1; }
inline int cvw_d(void (*)(int), CVS_IL) { return 2; } inline int cvw_d(bool, CVS_IL) { return 1; }
