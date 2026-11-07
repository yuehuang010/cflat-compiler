#pragma once
// T226 fixture: C++ values for static-local initialization. Every construction, copy, move and
// destruction is counted, so a leg can compare the counts with clang's. B is a base of D.
namespace t226s {
inline int made = 0, copies = 0, moves = 0, dead = 0, conv = 0;
struct T {
    int v;
    T(int x) : v(x) { made++; }
    T(const T& o) : v(o.v) { copies++; }
    T(T&& o) : v(o.v) { moves++; o.v = -1; }
    ~T() { dead++; }
    int get() const { return v; }
};
inline int take(T x) { return x.get(); }
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
// STATLEFT: a derived class with a user-declared copy constructor has no implicit move constructor.
struct Dc : B {
    int w;
    Dc(int x) : B(x), w(x * 10) {}
    Dc(const Dc& o) : B(o), w(o.w) {}
};
// STATLEFT round 2: converting constructors from the derived class (B(const D&); B(const D&) with
// B(D&&); explicit B(B&&)). `conv` counts converting-constructor bodies.
struct DK;
struct BK {
    int v;
    BK(int x) : v(x) { made++; }
    BK(const BK& o) : v(o.v) { copies++; }
    BK(BK&& o) : v(o.v) { moves++; o.v = -1; }
    BK(const DK& o);
    ~BK() { dead++; }
    int get() const { return v; }
};
struct DK : BK { DK(int x) : BK(x) {} };
inline BK::BK(const DK& o) : v(o.v) { conv++; }
struct DV;
struct BV {
    int v;
    BV(int x) : v(x) { made++; }
    BV(const BV& o) : v(o.v) { copies++; }
    BV(BV&& o) : v(o.v) { moves++; o.v = -1; }
    BV(const DV& o);
    BV(DV&& o);
    ~BV() { dead++; }
    int get() const { return v; }
};
struct DV : BV { DV(int x) : BV(x) {} };
inline BV::BV(const DV& o) : v(o.v) { conv += 1; }
inline BV::BV(DV&& o) : v(o.v) { conv += 10; }
struct BX {
    int v;
    BX(int x) : v(x) { made++; }
    BX(const BX& o) : v(o.v) { copies++; }
    explicit BX(BX&& o) : v(o.v) { moves++; o.v = -1; }
    ~BX() { dead++; }
    int get() const { return v; }
};
struct DX : BX { DX(int x) : BX(x) {} };
// T243: a class prvalue whose field is a D; `mkH(x).d` is an xvalue member of a temporary.
struct H {
    D d;
    H(int x) : d(x) {}
};
inline H mkH(int x) { return H(x); }
struct H2 {
    H h;
    H2(int x) : h(x) {}
};
struct HA {
    D a[2];
    HA(int x) : a{ D(x), D(x + 1) } {}
};
inline H2 mkH2(int x) { return H2(x); }
inline HA mkHA(int x) { return HA(x); }
inline void reset() { made = copies = moves = dead = conv = 0; }
}
