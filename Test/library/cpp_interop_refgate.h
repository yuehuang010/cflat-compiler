#pragma once

namespace cpprefgate {
struct C { int v; };
struct B {
    int v;
    B() : v(0) {}
    B(const B& other) : v(other.v) {}
    ~B() {}
};
struct Hold {
    C c{23};
    C& member() { return c; }
};
inline C& cell() { static C value{11}; return value; }
inline C& other_cell() { static C value{17}; return value; }
inline B& bref() { static B value; value.v = 29; return value; }
inline C& local_ref(C& value) { return value; }
}
