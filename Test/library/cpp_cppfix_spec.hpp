#pragma once
#include "cpp_cppfix.hpp"
namespace cppfix
{
    template<> inline int R<long> = 9;
    template<> int Q<long> = 9;
    inline int q_long_value() { return Q<long>; }
    template<> inline constexpr int V<long> = 5;
    inline int r_long_value() { return R<long>; }
    inline int* r_long_address() { return &R<long>; }
    template<class T> inline constexpr int W<T*> = 7;
    // Primary and specialization in one header.
    template<class T> inline constexpr int X = 20;
    template<> inline constexpr int X<char> = 21;
}
namespace cppfix_dep
{
    template<> inline constexpr int W<long> = 5;
}
