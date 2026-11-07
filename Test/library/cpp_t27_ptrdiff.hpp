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

struct FloatAdd
{
    float value = 1.0f;
    float operator+(double d) const { return value + (float)d; }
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
template<class T> int integer_arith_kind(T) noexcept
{
    return std::is_same_v<T, int> ? 1 : std::is_same_v<T, unsigned int> ? 2
         : std::is_same_v<T, long> ? 3 : std::is_same_v<T, unsigned long> ? 4
         : std::is_same_v<T, long long> ? 5 : 0;
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

namespace t27ptr
{
template<class T> int bool_kind(T) noexcept
{
    return std::is_same_v<T, bool> ? 1 : std::is_same_v<T, int> ? 2 : 0;
}
inline int bool_pick(bool) { return 1; }
inline int bool_pick(int) { return 2; }
}

namespace t27ptr
{
enum T58Plain { T58A = 1, T58B = 2 };
enum T58Long : long { T58LA = 1, T58LB = 4 };
enum T58ULong : unsigned long { T58UA = 1, T58UB = 8 };

template<class T> constexpr int arithmetic_type_code() noexcept
{
    if constexpr (std::is_lvalue_reference_v<T>)
    {
        using Value = std::remove_cv_t<std::remove_reference_t<T>>;
        return std::is_same_v<Value, long> ? 23 : 20 + arithmetic_type_code<Value>();
    }
    else
    {
        using Value = std::remove_cv_t<T>;
        return std::is_same_v<Value, int> ? 1 : std::is_same_v<Value, unsigned int> ? 2
             : std::is_same_v<Value, long> ? 3 : std::is_same_v<Value, unsigned long> ? 4
             : std::is_same_v<Value, long long> ? 5 : std::is_same_v<Value, double> ? 6
             : std::is_same_v<Value, float> ? 7 : std::is_same_v<Value, char> ? 8
             : std::is_same_v<Value, signed char> ? 9 : std::is_same_v<Value, unsigned char> ? 10
             : std::is_same_v<Value, short> ? 11 : std::is_same_v<Value, unsigned short> ? 12
             : std::is_same_v<Value, bool> ? 13 : std::is_same_v<Value, unsigned long long> ? 14 : 99;
    }
}

template<class T> int arithmetic_value(T value) noexcept
{
    return 100 * arithmetic_type_code<T>() + static_cast<int>(value);
}
template<class T> int arithmetic_value_ref(T&& value) noexcept
{
    return 100 * arithmetic_type_code<T>() + static_cast<int>(value);
}
template<class T> int arithmetic_value_const(const T& value) noexcept
{
    return 100 * arithmetic_type_code<T>() + static_cast<int>(value);
}
// Writes through the forwarding reference, so a caller slot proves it was bound, not copied.
template<class T> int arithmetic_bump_ref(T&& value) noexcept
{
    value += 1;
    return 100 * arithmetic_type_code<T>() + static_cast<int>(value);
}

struct ArithmeticCtor
{
    int result;
    template<class T> explicit ArithmeticCtor(T value)
        : result(100 * arithmetic_type_code<T>() + static_cast<int>(value)) {}
    int get() const noexcept { return result; }
};

struct ArithmeticMember
{
    template<class T> int value(T value) const noexcept
    {
        return 100 * arithmetic_type_code<T>() + static_cast<int>(value);
    }
};

struct ArithmeticX {};
struct ArithmeticIntX {};
struct ArithmeticDoubleX {};
inline long operator-(long value, ArithmeticX) { return value + 700; }
inline unsigned long operator-(unsigned long value, ArithmeticX) { return value + 800; }
inline long operator*(ArithmeticX, long value) { return 900 + value; }
inline long operator*(ArithmeticX, unsigned long value) { return 1000 + static_cast<long>(value); }
inline int operator*(ArithmeticIntX, int value) { return 2000 + value; }
inline int operator*(ArithmeticDoubleX, double value) { return 3000 + static_cast<int>(value); }
}
