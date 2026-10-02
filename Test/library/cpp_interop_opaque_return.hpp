#pragma once

#include <type_traits>
#include <functional>
#include <utility>

namespace cppinterop {
inline int opaque_return_destructions = 0;

template<class F>
struct __callable_result {
    int& value;
    F function;
    ~__callable_result() { ++opaque_return_destructions; }
    int operator()(int amount) const { return value + function(amount); }
};

template<class F>
inline __callable_result<F> make_callable(int& value, F function) {
    return {value, std::move(function)};
}

inline int callable_destructions() { return opaque_return_destructions; }

// Hidden friends of a class template: found only by argument-dependent lookup, never
// declared at namespace scope, and only instantiated once CFlat names the specialization.
struct adl_scale { int factor; int operator()(int v) const { return v * factor; } };

template<class T>
struct adl_path {
    T depth;
    friend adl_path operator/(const adl_path& a, const adl_path& b) { return {a.depth * 10 + b.depth}; }
    // A constrained hidden friend: viable only for an integral T.
    friend int operator|(const adl_path& p, const adl_scale& s)
        requires std::is_integral_v<T>
    { return s(p.depth) + 7; }
    // A constrained hidden-friend template, viable for any callable taking T.
    template<class F>
        requires std::is_invocable_r_v<int, const F&, T>
    friend int operator^(const adl_path& p, const F& f) { return f(p.depth) + 9; }
    // Hidden-friend COMPOUND operators, class left and scalar left: C++ calls these and never
    // rewrites `a op= b` to `a = a op b` (the binary `+` friends below would give other values).
    friend int operator+(const adl_path& p, int n) { return p.depth + n + 5000; }
    friend int operator+(int n, const adl_path& p) { return p.depth + n + 6000; }
#define CPPINTEROP_ADL_COMPOUND(OP, K) \
    friend adl_path& operator OP(adl_path& a, int n) { a.depth = a.depth * 100 + n * 10 + K; return a; } \
    friend int& operator OP(int& n, const adl_path& p) { n = n * 100 + p.depth * 10 + K; return n; }
    CPPINTEROP_ADL_COMPOUND(+=, 1)
    CPPINTEROP_ADL_COMPOUND(-=, 2)
    CPPINTEROP_ADL_COMPOUND(*=, 3)
    CPPINTEROP_ADL_COMPOUND(/=, 4)
    CPPINTEROP_ADL_COMPOUND(%=, 5)
    CPPINTEROP_ADL_COMPOUND(<<=, 6)
    CPPINTEROP_ADL_COMPOUND(>>=, 7)
    CPPINTEROP_ADL_COMPOUND(&=, 8)
    CPPINTEROP_ADL_COMPOUND(|=, 9)
    CPPINTEROP_ADL_COMPOUND(^=, 10)
#undef CPPINTEROP_ADL_COMPOUND
};
template<class T>
inline adl_path<T> make_adl_path(T depth) { return {depth}; }
inline adl_scale make_adl_scale(int factor) { return {factor}; }
// operator int() plus exact-match hidden friends: clang picks the friend, never conversion + builtin.
inline int conv_box_conversions = 0;
template<class T>
struct conv_box {
    T v;
    operator int() const { ++conv_box_conversions; return v; }
    friend int operator+(const conv_box& b, int x) { return 700 + b.v + x; }
    friend int& operator+=(int& a, const conv_box& b) { a += 800 + b.v; return a; }
};
inline conv_box<int> make_conv_box(int v) { return {v}; }
inline int conv_box_conversion_count() { return conv_box_conversions; }
}

// ADL through a base class: the operator lives in the BASE's namespace or is the base's hidden
// friend; the derived class itself declares no friend operator.
namespace cppinterop_adl_base_ns {
struct Base { int v; };
inline int operator+(const Base& b, int x) { return 300 + b.v + x; }
}
namespace cppinterop_adl_base_friend {
struct Base { int v; friend int operator+(const Base& b, int x) { return 400 + b.v + x; } };
}
namespace cppinterop_adl_derived {
struct FromNs : cppinterop_adl_base_ns::Base {};
struct FromFriend : cppinterop_adl_base_friend::Base {};
inline FromNs make_from_ns(int v) { FromNs d; d.v = v; return d; }
inline FromFriend make_from_friend(int v) { FromFriend d; d.v = v; return d; }
}
