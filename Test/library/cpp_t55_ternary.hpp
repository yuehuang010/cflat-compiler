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
struct AssignNarrowUnsigned {
    int c = 0;
    AssignNarrowUnsigned() = default;
    AssignNarrowUnsigned(unsigned value) { c = 4000 + (int)value; }
};
struct AssignNarrowExplicit {
    int c = 0;
    AssignNarrowExplicit() = default;
    explicit AssignNarrowExplicit(int value) { c = 1000 + value; }
    AssignNarrowExplicit(signed char value) { c = 3000 + (int)value; }
};
struct AssignNarrowRoute {
    int c = 0;
    int via = 0;
    AssignNarrowRoute() = default;
    AssignNarrowRoute(unsigned value) { c = 4000 + (int)value; via = 1; }
    AssignNarrowRoute& operator=(int value) { c = 1000 + value; via = 2; return *this; }
};
struct AssignNarrowDeleted {
    int c = 0;
    AssignNarrowDeleted(unsigned value) { c = 4000 + (int)value; }
    AssignNarrowDeleted& operator=(const AssignNarrowDeleted&) = delete;
};
struct AmbSB { int c; AmbSB(signed char) : c(1) {} AmbSB(bool) : c(2) {} };
struct AmbSL { int c; AmbSL(signed char) : c(1) {} AmbSL(long) : c(2) {} };
struct AmbSY { int c; AmbSY(signed char) : c(1) {} AmbSY(unsigned) : c(2) {} };
struct AmbSF { int c; AmbSF(float) : c(1) {} AmbSF(int) : c(2) {} };
struct AmbSH { int c; AmbSH(signed char) : c(1) {} AmbSH(long long) : c(2) {} };
inline AssignNarrowExplicit assignNarrowGlobal;
}
