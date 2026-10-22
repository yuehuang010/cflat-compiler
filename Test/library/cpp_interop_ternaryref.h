#pragma once

namespace ternref {
struct C { int v; };
struct Derived : C { int tag; };

inline C& ref_a() { static C a{1}; return a; }
inline C& ref_b() { static C b{2}; return b; }
inline C& ref_c() { static C c{3}; return c; }
inline Derived& ref_derived() { static Derived d{{4}, 40}; return d; }
inline C value_a() { return C{5}; }
inline C value_b() { return C{6}; }
inline void write(C* p, int value) { p->v = value; }
// T391: a counted class for `c ? default : x` arguments to C++ free functions and members.
inline int* t391_counts() { static int c[5] = { 0, 0, 0, 0, 0 }; return c; }
inline void t391_reset() { for (int i = 0; i < 5; ++i) t391_counts()[i] = 0; }
// ctor, copy, move, dtor, live allocations, packed one decimal digit each.
inline int t391_stats()
{
    int* c = t391_counts();
    return c[0] * 10000 + c[1] * 1000 + c[2] * 100 + c[3] * 10 + c[4];
}
struct T391Val
{
    int* p;
    T391Val() : p(new int(0)) { ++t391_counts()[0]; ++t391_counts()[4]; }
    explicit T391Val(int n) : p(new int(n)) { ++t391_counts()[0]; ++t391_counts()[4]; }
    T391Val(const T391Val& o) : p(new int(*o.p)) { ++t391_counts()[1]; ++t391_counts()[4]; }
    T391Val(T391Val&& o) noexcept : p(o.p) { o.p = nullptr; ++t391_counts()[2]; }
    ~T391Val() { if (p != nullptr) --t391_counts()[4]; delete p; ++t391_counts()[3]; }
    int value() const { return p != nullptr ? *p : -1; }
};
// Converts to T391Val through a user conversion operator (a different class arm).
struct T391Zero
{
    int field;
    T391Zero() : field(0) {}
    operator T391Val() const { return T391Val(); }
};
inline int t391_take(T391Val v) { return v.value(); }
inline int t391_tref(const T391Val& v) { return v.value(); }
struct T391Host
{
    int tref(const T391Val& v) { return 30 + v.value(); }
    int take(T391Val v) { return 40 + v.value(); }
};
}
