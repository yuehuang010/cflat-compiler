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
}
