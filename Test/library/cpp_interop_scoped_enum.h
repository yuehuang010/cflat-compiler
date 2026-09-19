#pragma once

#include <map>
#include <vector>

namespace cppi_scope
{
    enum class A : int { x = 7 };
    enum class B : int { y = 7 };
    // Fixed underlying type: an unfixed one is 'unsigned int' on Itanium but 'int' on MSVC.
    enum U1 : unsigned int { u = 7 };
    enum U2 { w = 7 };

    template <typename T>
    struct KindTag;
    template <>
    struct KindTag<A> { static constexpr int value = 2; };
    template <>
    struct KindTag<int> { static constexpr int value = 1; };
    template <>
    struct KindTag<unsigned int> { static constexpr int value = 1; };
    template <>
    struct KindTag<U1> { static constexpr int value = 3; };

    template <typename T>
    inline int deduceKind(T) { return KindTag<T>::value; }

    struct Dev
    {
        int value;
        Dev(B b) : value((int)b + 100) {}
    };

    struct DevA
    {
        int value;
        DevA(A a) : value((int)a + 200) {}
    };

    struct Module
    {
        int pick(A a) const { return 1100 + (int)a; }
        int pick(Dev d) const { return 1200 + d.value; }
        int only(Dev d) const { return 1300 + d.value; }
        int onlyB(B b) const { return 1400 + (int)b; }
        int ref(const A& a) const { return 1500 + (int)a; }
        int mut(A& a) { return 1600 + (int)a; }
        template <typename T>
        int deduceKind(T) const { return KindTag<T>::value + 2; }
    };

    inline int pick(A a) { return 1000 + (int)a; }
    inline int pick(Dev d) { return 2000 + d.value; }
    inline int onlyDev(Dev d) { return 3000 + d.value; }
    inline int onlyInt(int v) { return 4000 + v; }
    inline int onlyB(B b) { return 5000 + (int)b; }
    inline int devA(DevA d) { return 6000 + d.value; }
    inline int ref(const A& a) { return 7000 + (int)a; }
    inline int mut(A& a) { return 8000 + (int)a; }
    inline int unscopedInt(int v) { return 9000 + v; }
    inline int unscopedOther(U2 u) { return 10000 + (int)u; }
    inline int takeAVector(const std::vector<A>&) { return 8; }
    inline int takeAMap(const std::map<A, int>&) { return 8; }
    inline int takeUIntVector(const std::vector<unsigned int>&) { return 8; }
    inline int takeUIntMap(const std::map<int, unsigned int>&) { return 8; }

    // Scoped-enum NON-TYPE template parameters. A scoped enumeration has no implicit conversion
    // from its integer, so the CFlat argument has to reach clang as a cast to the enum type.
    enum class Hue : unsigned char { Red = 1, Green = 2, Deep = 200 };
    enum class Mode { Off = 0, On = 1 };          // no explicit underlying type
    enum class Sign : int { Neg = -3, Pos = 3 };
    enum PlainN { PN_TWO = 2 };

    template <Hue H>    struct HueBox   { int get() const { return (int)H; } };
    template <Mode M>   struct ModeBox  { int get() const { return (int)M; } };
    template <Sign S>   struct SignBox  { int get() const { return (int)S; } };
    template <PlainN P> struct PlainBox { int get() const { return (int)P; } };
    template <int N>    struct IntBox   { int get() const { return N; } };
    template <bool B>   struct BoolBox  { int get() const { return B ? 41 : 17; } };
    template <char C>   struct CharBox  { int get() const { return (int)C; } };
    template <typename T> struct HueWrap { T inner; int get() const { return inner.get() + 1000; } };
    template <typename T> struct HueTag { T v; HueTag(T x) : v(x) {} int as_int() const { return (int)v; } };

    struct Host { enum class Kind : int { A = 5, B = 6 }; };
    template <Host::Kind K> struct HostBox { int get() const { return (int)K; } };

    using DeepHueBox = HueBox<Hue::Deep>;
    template <Hue H> using HueAlias = HueBox<H>;

    namespace deep
    {
        enum class Tone : int { T1 = 11, T2 = 12 };
        template <Tone T> struct ToneBox { int get() const { return (int)T; } };
    }

    inline int takeGreenBox(HueBox<Hue::Green> b) { return b.get() + 500; }
}
