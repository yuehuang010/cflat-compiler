#pragma once
// Primary variable templates. Imported on its own line BEFORE cpp_cppfix_spec.hpp, which
// supplies the explicit and partial specializations (a later import must still win).
namespace cppfix
{
    template<class T> inline constexpr int V = 3;
    template<class T> inline constexpr int W = 1;
    template<class T> inline int R = 4;
    template<class T> inline constexpr int AV = V<T> + 100;
    template<class T> int Q = 1;
    template<class T> inline int S = 2;
    template<> inline int S<long> = 8;
    template<class T> inline const int K = (int)sizeof(T) * 5;
}
namespace cppfix_dep
{
    template<class T> inline constexpr int W = 1;
    template<class T> inline constexpr int V = W<T> + 10;
    template<class T> inline constexpr int AV = V<T> + 100;
}
