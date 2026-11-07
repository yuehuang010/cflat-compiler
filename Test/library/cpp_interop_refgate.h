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
    C& via(C** value) { return **value; }
    C& viar(C*& value) { return *value; }
};
struct Callable { C& operator()(C** value) { return **value; } };
struct DoublePointerHolder { C** pp = nullptr; C& get() { return **pp; } };
inline C& cell() { static C value{11}; return value; }
inline C& other_cell() { static C value{17}; return value; }
inline C** heap_double_pointer_holder() {
    static C object{83};
    static C* objectPointer = &object;
    static C** holder = new C*(objectPointer);
    return holder;
}
inline C** pass_double_pointer(C** value) { return value; }
inline B& bref() { static B value; value.v = 29; return value; }
inline C& local_ref(C& value) { return value; }
inline C& pointer_ref(C* value) { return *value; }
inline C& value_ref(C value) { static C result; result = value; return result; }
inline C& pointer_ref_ref(C*& value) { return *value; }
inline const C& const_ref(const C& value) { return value; }
inline int pointer_cv_pick(C*) noexcept { return 1; }
inline int pointer_cv_pick(const C*) noexcept { return 2; }
inline C& double_pointer_ref(C** value) { return **value; }
}
