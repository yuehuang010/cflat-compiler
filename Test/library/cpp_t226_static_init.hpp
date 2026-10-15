#pragma once
// T226 fixture: C++ values for static-local initialization. Every construction, copy, move and
// destruction is counted, so a leg can compare the counts with clang's. B is a base of D.
namespace t226s {
inline int made = 0, copies = 0, moves = 0, dead = 0;
struct T {
    int v;
    T(int x) : v(x) { made++; }
    T(const T& o) : v(o.v) { copies++; }
    T(T&& o) : v(o.v) { moves++; o.v = -1; }
    ~T() { dead++; }
    int get() const { return v; }
};
struct B {
    int v;
    B(int x) : v(x) { made++; }
    B(const B& o) : v(o.v) { copies++; }
    B(B&& o) : v(o.v) { moves++; o.v = -1; }
    ~B() { dead++; }
    int get() const { return v; }
};
struct D : B {
    int w;
    D(int x) : B(x), w(x * 10) {}
};
// T243: a class prvalue whose field is a D; `mkH(x).d` is an xvalue member of a temporary.
struct H {
    D d;
    H(int x) : d(x) {}
};
inline H mkH(int x) { return H(x); }
inline void reset() { made = copies = moves = dead = 0; }
}
