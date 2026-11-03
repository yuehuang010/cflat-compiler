#pragma once

// Member template compound operator: `template<class U> T& operator&=(U&&)` is instantiated for the
// receiver and the operand (an lvalue or a `move`), as clang instantiates it.
namespace mvops {
struct TP4 {
    int v;
    TP4(int x = 0) : v(x) {}
    template<class U> TP4& operator&=(U&& o) { v += o.v; return *this; }
};

// Counted class for by-value operator results used as an initializer: the result constructs into
// the declared object (no move, one copy for the returned *this, as clang elides it).
struct Cnt {
    int v;
    static inline int cp = 0, mv = 0, dt = 0;
    Cnt(int x = 0) : v(x) {}
    Cnt(const Cnt& o) : v(o.v) { ++cp; }
    Cnt(Cnt&& o) noexcept : v(o.v) { o.v = 0; ++mv; }
    ~Cnt() { ++dt; }
    static void reset() { cp = mv = dt = 0; }
    Cnt operator*=(int n) { v *= n; return *this; }
};
inline Cnt mkCnt(int x) { return Cnt(x); }

// Copy-only class (no move constructor): a by-value free compound result chained into another
// compound. Both results bind or construct without an extra copy, as clang does.
struct Cpy {
    int v = 0;
    static inline int cp = 0;
    Cpy() {}
    Cpy(int x) : v(x) {}
    Cpy(const Cpy& o) : v(o.v) { ++cp; }
    static void reset() { cp = 0; }
};
inline Cpy operator-=(Cpy& f, int n) { f.v -= n; return f; }
inline Cpy operator|=(const Cpy& f, int n) { Cpy r(f); r.v |= n; return r; }
inline Cnt operator-=(Cnt& f, int n) { f.v -= n; return f; }
inline Cnt operator|=(const Cnt& f, int n) { Cnt r(f); r.v |= n; return r; }
}
