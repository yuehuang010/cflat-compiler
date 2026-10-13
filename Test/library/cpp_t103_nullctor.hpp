#pragma once
#include <memory>
#include <functional>
#define T103_NULL_ALIAS NULL
#define T103_NULLPTR_MACRO nullptr

namespace t103 {
inline int only_int(int) { return 11; }
inline int only_pointer(int* p) { return p == nullptr ? 22 : 23; }
inline int pointer_or_long(int* p) { return p == nullptr ? 31 : 33; }
inline int pointer_or_long(long) { return 32; }
inline int c_header_null(int* p) { return p == nullptr ? 41 : 42; }
inline int macro_null(int* p) { return p == nullptr ? 43 : 44; }
inline int macro_nullptr(int* p) { return p == nullptr ? 45 : 46; }
inline int empty_function(std::function<int()> f) { return f ? 47 : 48; }
struct NonLiteralZero {
    int v;
    NonLiteralZero(int, const double&) : v(11) {}
    NonLiteralZero(int*, const double&) : v(99) {}
};
struct BraceZero {
    int v;
    BraceZero(short, int) : v(11) {}
    BraceZero(int*, double) : v(99) {}
};
struct BraceOuter { BraceZero z; };
// No implicit narrowing at a constructor argument, also where clang accepts it.
struct NarrowMixed { int v; NarrowMixed(int x) : v(x) {} NarrowMixed(const int&, int) : v(9) {} };
struct NarrowPointer {
    int v;
    NarrowPointer(int x) : v(x) {}
    NarrowPointer(const int&, int) : v(9) {}
    NarrowPointer(int*) : v(99) {}
};
struct NarrowDead {
    int v;
    NarrowDead(int x) : v(x) {}
    NarrowDead(const int&, int) : v(9) {}
    NarrowDead(int*, double) : v(99) {}
};
struct NarrowPair { int v; NarrowPair(int x, const int&) : v(x) {} NarrowPair(int*, double) : v(99) {} };
// C++ selects (int, char) for (short, int): promotion beats conversion at argument 1.
struct NarrowRanked { int v; NarrowRanked(int, char) : v(1) {} NarrowRanked(const long&, long) : v(2) {} };
inline int zero_pick(int) { return 11; }
inline int zero_pick(int*) { return 99; }
enum E { zero = 0 };
struct Owners {
    int unique_empty(std::unique_ptr<int> p) { return p ? 51 : 52; }
    int shared_empty(std::shared_ptr<int> p) { return p ? 61 : 62; }
};
}
