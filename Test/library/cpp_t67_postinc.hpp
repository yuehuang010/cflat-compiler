#pragma once
#include <initializer_list>

namespace cpp_t67 {
inline int by_value(int value) { return 100 + value; }
inline int by_const_ref(const int& value) { return 200 + value; }
inline int by_rvalue_ref(int&& value) { return 300 + value; }
template<class T> inline int by_template(T value) { return 400 + value; }
inline long ptr_const_ref(int* const& value, int* base) { return value - base; }
inline long ptr_rvalue_ref(int*&& value, int* base) { return value - base; }
inline long ptr_value(int* value, int* base) { return value - base; }
inline int* postinc_base = nullptr;

struct Pair { int first, second; };
struct PtrMade {
    int* value;
    PtrMade(int* const& p) : value(p) {}
};
struct PtrOps {
    long member(int* const& p, int* base) const { return p - base; }
    long operator+(int* const& p) const { return p - postinc_base; }
};
struct Made {
    int first, second;
    Made(int a, int b) : first(a), second(b) {}
};
struct List {
    int first, second;
    List(std::initializer_list<int> values) : first(0), second(0) {
        auto it = values.begin();
        if (it != values.end()) first = *it++;
        if (it != values.end()) second = *it;
    }
};
struct Obj { int call(int value) { return 500 + value; } };
struct Add { int operator+(int value) const { return 600 + value; } };
inline int take(std::initializer_list<int> values) {
    auto it = values.begin();
    int first = *it++;
    return first * 10 + *it;
}
}
