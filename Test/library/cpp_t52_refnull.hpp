#pragma once
#include <array>
#include <cstddef>

#define ZERO 0UL
// 64-bit on every target (0UL is 32-bit on LLP64): operator legs must not hit int -> pointer.
#define ZERO64 0ULL
#define CHZERO '\0'
#define M1 (1-1)
#define M2 ((int)0)
#define M3 (0 << 3)
#define M4 false
#define ALIASZERO ZERO
#define PARENZERO (0)
#define HEXZERO 0x0
#define Z0 0
#define N5 5

namespace cpi_t52 {
inline int av(int (&a)[2]) { ++a[0]; return a[0]; }
inline int fr(int (&)(int[2])) { return 15; }
inline int nested(std::array<int, 2>&) { return 16; }
inline int lp(int* p) { return p == nullptr ? 5 : 6; }
inline int pref(int* const& p) { return p == nullptr ? 27 : 28; }
inline int prvalue(int*&& p) { return p == nullptr ? 29 : 30; }
inline int nref(const std::nullptr_t& p) { return p == nullptr ? 31 : 32; }
inline int dirty() { volatile long long a[8]; for (int i = 0; i < 8; ++i) a[i] = -1; return (int)a[3]; }
inline int plref(int*& p) { return p == nullptr ? 33 : 34; }
inline int iref(int& v) { return v + 10; }
inline int irr(int&& v) { return v + 40; }
// A zero macro is a prvalue: T deduces int, not int& (std::setlocale(LC_ALL, ...) shape).
template <class T> int fwd(T&& v) { return __is_lvalue_reference(T) ? 80 + (int)v : 90 + (int)v; }
// A null constant converts implicitly through a converting ctor taking a pointer.
struct K { int v; K(int* p) : v(p == nullptr ? 5 : 6) {} };
inline int tk(K k) { return k.v; }
enum E { ez = 0 };
}
