#pragma once
#include "cpp_cppfix.hpp"
namespace cppfix
{
    template<> inline constexpr int V<long> = 5;
    template<class T> inline constexpr int W<T*> = 7;
    // Primary and specialization in one header.
    template<class T> inline constexpr int X = 20;
    template<> inline constexpr int X<char> = 21;
}
