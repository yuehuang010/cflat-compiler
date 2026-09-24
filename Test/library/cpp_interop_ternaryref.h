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
}
