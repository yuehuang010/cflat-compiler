#pragma once

namespace t3lookup {
inline int g(long) { return 1; }
inline int f1() { return g(1); }
inline int g(int) { return 2; }

inline int h(long) { return 1; }
inline int f2() { return h(1); }
template <class T> int h(T) { return 2; }

inline int k(long) { return 1; }
inline int f3() { return k(1); }
namespace N { inline int k(int) { return 2; } }
using N::k;

inline int m(long) { return 1; }
struct S { int get() { return m(1); } };
inline int m(int) { return 2; }
inline int f4() { S s; return s.get(); }

struct A {};
template <class T> int t(T x) { return q(x); }
inline int q(A) { return 3; }
inline int f5() { return t(A{}); }

extern "C" inline int* find_value(const void* data, int)
{
    return const_cast<int*>(static_cast<const int*>(data));
}
inline const int* find_value(const char*, int);
inline int f6(const char* d) { return find_value(d, 0) == nullptr ? 6 : 7; }
inline const int* find_value(const char*, int) { return nullptr; }

inline int z(double) { return 1; }
inline int f7() { return z(1); }
inline int z(int) { return 2; }

inline int d(int a, int b) { return a + b; }
inline int f8() { return d(1, 2); }

struct B { int v; };
extern "C" inline int cf(const void*) { return 1; }
template <class T> int tt(T* x) { return cf(x); }
inline int cf(B*) { return 2; }
inline int fp() { B b{0}; return tt(&b); }

extern "C" inline int cg(const void*) { return 1; }
struct C { int v; friend int cg(C*); };
inline int fp2() { C c{0}; return cg(&c); }
inline int cg(C*) { return 2; }

// Lambda / local-class bodies: inner callees keep point-of-definition lookup;
// declarations inside the body itself are never "later" (no recovery parse).
inline int l9(long) { return 1; }
inline int f9() { auto fn = []() { return l9(1); }; return fn(); }
inline int l9(int) { return 2; }

inline int f10() { auto fn = [](int x) { return x + 1; }; return fn(0); }

inline int lc(long) { return 1; }
inline int f11() { struct L { int go() { return lc(1); } }; return L{}.go(); }
inline int lc(int) { return 2; }

inline int f12() { struct L2 { int one() { return two() - 1; } int two() { return 3; } }; return L2{}.one(); }
}
