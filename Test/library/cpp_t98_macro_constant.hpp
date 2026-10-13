#pragma once
#include <initializer_list>

// Imported macro constants are C++ prvalues; the enum and brace shapes are constants in C++.
#define T98_N5 5
#define T98_SMALL 5
#define T98_BIG 70000
#define T98_UMAX 4294967295U
#define T98_ULLMAX 18446744073709551615ULL
#define T98_CH 'A'

namespace cpi_t98 {
enum Color { Red = 1, Green = 2 };
inline int ov(int& v) { return 100 + v; }
inline int ov(int&& v) { return 200 + v; }
inline int iref(int& v) { return v + 10; }
inline int irr(int&& v) { return v + 40; }
inline int cref(const int& v) { return v + 1; }
template <class T> int fwd(T&& v) { return __is_lvalue_reference(T) ? 80 + (int)v : 90 + (int)v; }
// Shaped like std::vector: an initializer_list constructor over the element type.
struct Shorts {
    int n; int first;
    Shorts(std::initializer_list<short> l) : n((int)l.size()), first(*l.begin()) {}
};
struct Ints {
    int n; int first;
    Ints(std::initializer_list<int> l) : n((int)l.size()), first(*l.begin()) {}
};
// Overloads keyed on the constant's own type: int 11, unsigned long long 22, char 33, Color 44.
struct Pick {
    int n;
    Pick(int) : n(11) {}
    Pick(unsigned long long) : n(22) {}
    Pick(char) : n(33) {}
    Pick(Color) : n(44) {}
};
struct Ulongs {
    unsigned long long n;
    Ulongs(std::initializer_list<unsigned long long> l) : n(*l.begin()) {}
};
}
