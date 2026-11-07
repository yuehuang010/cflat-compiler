#pragma once
#include <iterator>
#include <type_traits>

namespace castglob {
struct Box {
    int value;
    Box(int x) : value(x) {}
    Box(const char* p) : value(static_cast<unsigned char>(p[0])) {}
    int get() const { return value; }
};
inline Box makeBox(int x) { return Box(x); }

using RefFn = char (&(*)())[4];
inline char (&buffer())[4] {
    static char value[4] = {97, 98, 99, 0};
    return value;
}
inline RefFn getfn() { return &buffer; }
inline int callfn(RefFn f) { return static_cast<unsigned char>(f()[0]); }
template<class T> inline int tref(T& a) {
    if constexpr (std::is_array_v<T>) return 12;
    else return -12;
}
inline int take_ptr_ref(int*& p) { int value = p[0]; p = nullptr; return value; }
inline int take_const_ptr_ref(int* const& p) { return p[0]; }
inline int take_rvalue_ptr_ref(int*&& p) { return p[0]; }
inline int take_const_int_ptr_ref(const int*& p) { return p[0]; }
inline int take_ptr_value(int* p) { return p[0]; }
struct Fn {
    int value;
    Fn(RefFn f) : value(static_cast<unsigned char>(f()[0])) {}
    int get() const { return value; }
};

using Ref2Fn = char (&(*)())[2][3];
inline char (&buffer2())[2][3] {
    static char value[2][3] = {{1, 2, 3}, {4, 5, 6}};
    return value;
}
inline Ref2Fn get2() { return &buffer2; }
inline int use2(Ref2Fn f) { return static_cast<unsigned char>(f()[1][2]); }
struct F2 {
    int value;
    F2(Ref2Fn f) : value(static_cast<unsigned char>(f()[1][1])) {}
    int get() const { return value; }
};

inline int plus100(int x) { return x + 100; }
inline int (*getcb())(int) { return plus100; }
struct FnCtorPick {
    int value;
    FnCtorPick(int (*f)(int)) : value(f(3)) {}
    FnCtorPick(long) : value(-1) {}
    int get() const { return value; }
};

template<class T> inline int tref_const(T& a) {
    if constexpr (std::is_array_v<T> && std::is_const_v<std::remove_extent_t<T>>) return 112;
    else return -112;
}
}

namespace fnrefctor {
template<class T> inline int tpr(T*& p) { return 1; }

template<class T> inline int tget(T& a) {
    if constexpr (std::rank_v<T> == 1) return (int)a[2] + 10 * (int)std::extent_v<T, 0>;
    else if constexpr (std::rank_v<T> == 2)
        return 100 + 10 * (int)std::extent_v<T, 0> + (int)std::extent_v<T, 1> + 0 * a[0][0];
    else return 3;
}

template<class T> inline int tptr(T* p) { return (int)sizeof(T); }

template<class T> inline int tref(T& a) {
    if constexpr (std::is_array_v<T>) return (int)sizeof(T);
    else return 3;
}
}
