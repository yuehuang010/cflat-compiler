#pragma once
// T87: a literal 0 (any integer suffix, or a zero macro) is a null pointer constant to clang, so it
// converts to a pointer parameter as a conversion, ranked beside the integral one.
#define T87_ZERO 0
#define T87_ZERO_L 0L
namespace t87 {
struct P { int v; P(short, int) : v(11) {} P(int*, double) : v(99) {} };
struct S2 { int v; S2(int) : v(11) {} S2(int*) : v(99) {} };
struct S4 { int v; S4(int, double) : v(11) {} S4(int*, double) : v(99) {} };
struct S6 { int v; S6(short) : v(11) {} S6(int*) : v(99) {} };
struct M { int m(short, int) { return 11; } int m(int*, double) { return 99; } };
inline int f(short, int) { return 11; }
inline int f(int*, double) { return 99; }
struct R1 { int v; R1(const int&, double) : v(11) {} R1(int*, const double&) : v(99) {} };
struct NullRef { int v; NullRef(short, const int&) : v(11) {} NullRef(int*, const double&) : v(99) {} };
struct R2 { int v; R2(const int&, int*) : v(11) {} R2(int*, double) : v(99) {} };
}
