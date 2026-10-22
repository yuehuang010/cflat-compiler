#pragma once

template <class T = int>
struct T358Gate
{
    T value{};
    int get() const { return (int)value; }
};

namespace t358
{
    inline int take(T358Gate<int> value) { return 1000 + value.get(); }
    inline int pick(T358Gate<int> value) { return 11 + value.get(); }
    inline int pick(T358Gate<long> value) { return 22 + value.get(); }
    inline T358Gate<int> make_gate(int value)
    {
        T358Gate<int> result{};
        result.value = value;
        return result;
    }
    inline T358Gate<long> make_long_gate(long value)
    {
        T358Gate<long> result{};
        result.value = value;
        return result;
    }

    template <class A, class B = int>
    struct Pair { A first{}; B second{}; };
    inline int pick_pair(Pair<float, int>) { return 33; }
    inline int pick_pair(Pair<float, long>) { return 44; }

    template <class T, class U = T>
    struct Same { T first{}; U second{}; };
    inline int pick_same(Same<int, int>) { return 55; }
    inline int pick_same(Same<int, long>) { return 66; }

    template <int N = 3>
    struct Count { int value() const { return N; } };
    inline int pick_count(Count<3>) { return 77; }
    inline int pick_count(Count<4>) { return 88; }

    template <class T>
    struct Outer { T value{}; };
    inline int pick_outer(Outer<T358Gate<int>>) { return 99; }
    inline int pick_outer(Outer<T358Gate<long>>) { return 100; }
}

inline int t358TakeDefault(T358Gate<int> value) { return 1000 + value.get(); }

namespace t358
{
    template <int N = 2 + 2>
    struct T358C4 { int value() const { return N; } };
    inline int pick_c4(T358C4<4>) { return 41; }
    inline int pick_c4(T358C4<5>) { return 42; }

    inline constexpr int t358KDef = 7;
    template <int N = t358KDef>
    struct T358CK { int value() const { return N; } };
    inline int pick_ck(T358CK<7>) { return 51; }
    inline int pick_ck(T358CK<8>) { return 52; }

    template <bool B = true>
    struct T358Flag { bool value() const { return B; } };
    inline int pick_flag(T358Flag<true>) { return 61; }
    inline int pick_flag(T358Flag<false>) { return 62; }

    template <int N = 1 + 2>
    struct T358CThree { int value() const { return N; } };
    inline int pick_three(T358CThree<3>) { return 43; }
    inline int pick_three(T358CThree<4>) { return 44; }

    template <class T = const char*>
    struct T358GP { T value{}; };
    inline int pick_gp(T358GP<const char*>) { return 31; }
    inline int pick_gp(T358GP<char*>) { return 32; }

    template <class T, class U = const T*>
    struct T358DepConst { U value{}; };
    inline int pick_dep_const(T358DepConst<int, const int*>) { return 37; }
    inline int pick_dep_const(T358DepConst<int, int*>) { return 38; }

    template <int N, int M = N>
    struct T358DepValue { int value() const { return M; } };
    inline int pick_dep_value(T358DepValue<4, 4>) { return 57; }
    inline int pick_dep_value(T358DepValue<4, 5>) { return 58; }

    enum T358Color { T358Red, T358Green = 5 };
    template <T358Color K = T358Green>
    struct T358EN { int x{}; };
    inline int pick_en(T358EN<T358Green>) { return 5; }
    inline int pick_en(T358EN<T358Red>) { return 6; }

    enum class T358Mode : int { A = 1, B = 2 };
    template <T358Mode M = T358Mode::B>
    struct T358EC { int x{}; };
    inline int pick_ec(T358EC<T358Mode::B>) { return 7; }
    inline int pick_ec(T358EC<T358Mode::A>) { return 8; }
}
