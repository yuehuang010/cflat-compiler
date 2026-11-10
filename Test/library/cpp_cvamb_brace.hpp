#pragma once
#include <initializer_list>
// CVAMB: braced and plain argument overloads that clang ranks per argument. The tied pairs are refusals
// in Test/errors/err_cpp_brace_overload_ambiguous.cb; the receiver and per-argument picks are value legs.
inline int cvb_pb2(std::initializer_list<long long>, std::initializer_list<long long>) { return 1; }
inline int cvb_pb2(std::initializer_list<double>, std::initializer_list<double>) { return 2; }
inline int cvb_pa(int, std::initializer_list<long long>) { return 1; }
inline int cvb_pa(int, std::initializer_list<double>) { return 2; }
inline int cvb_pd(std::initializer_list<long>, long) { return 1; }
inline int cvb_pd(std::initializer_list<long>, int) { return 2; }
inline int cvb_da(std::initializer_list<long>, int = 0) { return 1; }
inline int cvb_da(std::initializer_list<double>) { return 2; }
inline int cvb_fl(std::initializer_list<float>) { return 1; }
inline int cvb_fl(std::initializer_list<long double>) { return 2; }
struct CvbM
{
    int mq(int, std::initializer_list<long long>) { return 1; }
    int mq(int, std::initializer_list<double>) { return 2; }
    int mc(std::initializer_list<long>) const { return 1; }
    int mc(std::initializer_list<double>) { return 2; }
    int mrr(std::initializer_list<long>) const & { return 1; }
    int mrr(std::initializer_list<double>) & { return 2; }
    int mvq(std::initializer_list<long>) volatile { return 1; }
    int mvq(std::initializer_list<double>) { return 2; }
    int mbb(std::initializer_list<long>) & { return 1; }
    int mbb(std::initializer_list<double>) & { return 2; }
};
// CVAMB r1: reference parameters beside a braced list, a literal that cannot bind int&, a pointer tie with a
// defaulted parameter, and a reference member (clang picks as the expectations say; accept legs only).
inline int cvb_ref1(int&, std::initializer_list<long>) { return 1; }
inline int cvb_ref1(const int&, std::initializer_list<long>) { return 2; }
inline int cvb_nv(long, std::initializer_list<long>) { return 2; }
inline int cvb_nv(int&, std::initializer_list<long>) { return 1; }
inline int cvb_dp(const int*, std::initializer_list<long>, int = 0) { return 1; }
inline int cvb_dp(int*, std::initializer_list<long>) { return 2; }
struct CvbR
{
    int f(int&, std::initializer_list<long>) { return 1; }
    int f(const int&, std::initializer_list<long>) { return 2; }
};
// CVAMB r2: a pointer lvalue binds T*& beside T* const& (clang 1).
inline int cvb_pref(int*&, std::initializer_list<long>) { return 1; }
inline int cvb_pref(int* const&, std::initializer_list<long>) { return 2; }
