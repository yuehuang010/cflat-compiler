#pragma once
#include <type_traits>

// Member template compound operator: `template<class U> T& operator&=(U&&)` is instantiated for the
// receiver and the operand (an lvalue or a `move`), as clang instantiates it.
namespace mvops {
struct TP4 {
    int v;
    TP4(int x = 0) : v(x) {}
    template<class U> TP4& operator&=(U&& o) { v += o.v; return *this; }
};

struct TBin4 {
    int v;
    static inline int born = 0, dead = 0, copies = 0, moves = 0, picks = 0;
    TBin4(int x = 0) : v(x) { ++born; }
    TBin4(const TBin4& o) : v(o.v) { ++born; ++copies; }
    TBin4(TBin4&& o) noexcept : v(o.v) { o.v = 0; ++born; ++moves; }
    ~TBin4() { ++dead; }
    static void reset() { born = dead = copies = moves = picks = 0; }
    template<class U> TBin4 operator&(U&& o) const { ++picks; return TBin4(v + o.v); }
};

// Scalar lvalue operands into member forwarding-reference templates: U deduces int&, and the
// template beats a converting `const TBin5&` sibling; a plain `int` sibling wins the tie.
struct TBin5 {
    int v;
    static inline int born = 0, dead = 0, picks = 0;
    TBin5(int x = 0) : v(x) { ++born; }
    TBin5(const TBin5& o) : v(o.v) { ++born; }
    ~TBin5() { ++dead; }
    static void reset() { born = dead = picks = 0; }
    template<class U> TBin5 operator*(U&& o) const { picks = 4; return TBin5(v * o); }
    TBin5 operator|(const TBin5& o) const { picks = 1; return TBin5(v + o.v); }
    template<class U, std::enable_if_t<std::is_integral_v<std::decay_t<U>>, int> = 0>
    TBin5 operator|(U&& o) const { picks = 2; return TBin5(v + o + 100); }
    TBin5 operator^(const TBin5& o) const { picks = 1; return TBin5(v + o.v); }
    template<class U> requires std::is_integral_v<std::remove_cvref_t<U>>
    TBin5 operator^(U&& o) const { picks = 2; return TBin5(v + o + 200); }
    TBin5 operator+(int o) const { picks = 1; return TBin5(v + o); }
    template<class U, std::enable_if_t<std::is_integral_v<std::decay_t<U>>, int> = 0>
    TBin5 operator+(U&& o) const { picks = 2; return TBin5(v + o + 100); }
};

// A bitfield operand: `U&&` / `int&` deduce or declare a non-const reference, which clang refuses;
// `const U&` and by-value templates copy it. A pointer lvalue deduces `int*&` and reads through it.
struct TBits6 { int k = 7; int bits : 4 = 7; };
struct TBin6 {
    int v;
    static inline int born = 0, dead = 0, picks = 0;
    TBin6(int x = 0) : v(x) { ++born; }
    ~TBin6() { ++dead; }
    static void reset() { born = dead = picks = 0; }
    template<class U> int operator*(U&& o) const { picks = 6; o += 2; return v + o; }
    template<class U> int operator/(const U& o) const { picks = 7; return v + o; }
    template<class U> int operator%(U o) const { picks = 8; return v + o; }
    int operator-(int& o) const { picks = 9; o += 2; return v + o; }
    int operator+(int o) const { picks = 10; return v + o; }
    template<class U> int operator+(U&& o) const { picks = 11; return v + o + 100; }
    template<class U> int operator&(U&& o) const { picks = 12; return v + *o; }
    template<class U> int operator|(U&& o) const { picks = 13; o += 2; return v + o; }
    template<class U> int operator^(U&& o) const { picks = 14; o += 2; return v + o; }
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
