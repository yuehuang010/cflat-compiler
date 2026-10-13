#pragma once
#include <type_traits>
namespace t55 {
enum E { EA = 3 };
enum F { FA = 5 };
template<class T> int tf(T value) {
    if constexpr (std::is_same_v<T, int>) return 2000 + value;
    else if constexpr (std::is_same_v<T, long>) return 3000 + (int)value;
    else if constexpr (std::is_same_v<T, long long>) return 4000 + (int)value;
    else if constexpr (std::is_same_v<T, unsigned int>) return 5000 + (int)value;
    else if constexpr (std::is_same_v<T, E>) return 6000 + (int)value;
    else if constexpr (std::is_same_v<T, F>) return 7000 + (int)value;
    else if constexpr (std::is_same_v<T, double>) return 8000 + (int)(value * 10);
    else return 9000 + (int)value;
}
// T92: one code per C++ type (int 1000, long 2000, char 3000, signed char 4000, bool 5000,
// double 6000), so a constructor or operator= template deducing T is observable.
template<class T> int tk(T value) {
    if constexpr (std::is_same_v<T, int>) return 1000 + value;
    else if constexpr (std::is_same_v<T, long>) return 2000 + (int)value;
    else if constexpr (std::is_same_v<T, char>) return 3000 + (int)value;
    else if constexpr (std::is_same_v<T, signed char>) return 4000 + (int)value;
    else if constexpr (std::is_same_v<T, bool>) return 5000 + (int)value;
    else if constexpr (std::is_same_v<T, double>) return 6000 + (int)(value * 10);
    else return 9000 + (int)value;
}
struct CtorLeg {
    int c = 0;
    template<class T> CtorLeg(T value) { c = tk(value); }
};
struct AssignLeg {
    int last = 0;
    template<class T> AssignLeg& operator=(T value) { last = tk(value); return *this; }
};
// T92 round 2: the explicit int constructor is never a copy-initialization candidate; the
// non-explicit signed char one is chosen there, while direct initialization picks int.
struct ExplicitLeg {
    int c = 0;
    explicit ExplicitLeg(int value) { c = 1000 + value; }
    ExplicitLeg(signed char value) { c = 3000 + (int)value; }
};
}
