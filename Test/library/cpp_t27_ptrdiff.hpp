#pragma once
#include <type_traits>

namespace t27ptr
{
inline int pick_void_or_char(void*) { return 1; }
inline int pick_void_or_char(char*) { return 2; }
inline int pick_void_or_int(void*) { return 1; }
inline int pick_void_or_int(int*) { return 2; }
inline int read_int(int* p) { return *p; }

struct VoidCtor
{
    void* value;
    explicit VoidCtor(void* p) : value(p) {}
    int get() const { return value != nullptr ? 1 : 0; }
};

struct IntCtor
{
    int value;
    explicit IntCtor(void*) : value(1) {}
    explicit IntCtor(int* p) : value(*p) {}
    int get() const { return value; }
};

// 2 = int*, 3 = char*, 4 = double*, 1 = anything else.
template<class T> int kind(T) noexcept
{
    return std::is_same_v<T, int*> ? 2 : std::is_same_v<T, char*> ? 3
         : std::is_same_v<T, double*> ? 4 : 1;
}

// 2 = float, 3 = double, 1 = anything else.
template<class T> int arith_kind(T) noexcept
{
    return std::is_same_v<T, float> ? 2 : std::is_same_v<T, double> ? 3 : 1;
}
inline int pick_arith(float) { return 2; }
inline int pick_arith(double) { return 3; }

struct Kinds
{
    int base = 0;
    template<class T> int kind(T) noexcept
    {
        return base + (std::is_same_v<T, int*> ? 2 : std::is_same_v<T, char*> ? 3
                     : std::is_same_v<T, double*> ? 4 : 1);
    }
};
}
