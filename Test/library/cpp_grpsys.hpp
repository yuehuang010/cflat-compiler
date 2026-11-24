// Grouped `import cpp` with a standard-library header listed FIRST: every declaration kind of this
// later user header must still bind (test_cpp_interop.cb, grpsys legs).
#pragma once
#include <compare>
#define GRPSYS_MACRO 47
namespace grpsys {
struct Bin { int v; };
inline int operator+(const Bin& b, int x) { return b.v + x + 100; }
inline int operator-(const Bin& b) { return -b.v - 1000; }
inline Bin& operator+=(Bin& b, int x) { b.v += x * 2; return b; }
inline bool operator==(const Bin& a, const Bin& b) { return a.v == b.v + 1; }
// A reversed three-way result: `a < b` is true when a.v > b.v, only through this rewrite.
struct Cmp { int v; };
inline std::strong_ordering operator<=>(const Cmp& a, const Cmp& b) { return b.v <=> a.v; }
// The member needs double -> int, the free one is exact: clang picks the free one (61).
struct Prot {
protected:
    int operator+(int) const { return 60; }
};
inline int operator+(const Prot&, double) { return 61; }
struct Mem { int v; int operator*(int x) const { return v * x + 7; } };
inline int grpsysFree(int x) { return x * 3; }
template<class T> T grpsysTwice(T x) { return x + x + 1; }
enum class Color { Red = 3, Green = 9 };
enum Plain { GrpsysPlainA = 11 };
inline int grpsysGlobal = 23;
namespace inner { inline int deep() { return 31; } }
}
