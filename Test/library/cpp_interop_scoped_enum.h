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

    // Integer -> enumeration is never an implicit conversion in C++ (scoped or unscoped), so an
    // integer argument must pick the integer or converting-constructor overload, never the enum.
    inline int enumOrLong(A a) { return 11000 + (int)a; }
    inline int enumOrLong(long long v) { return 11100 + (int)v; }
    template <typename T> struct RefLike
    {
        const T* data; size_t len;
        RefLike(const T& one) : data(&one), len(1) {}
        template <size_t N> RefLike(const T (&arr)[N]) : data(arr), len(N) {}
    };
    struct Viewer
    {
        long long view(RefLike<long long> s) const { return 12000 + (long long)s.len * 100 + s.data[0]; }
        long long view(A a) const { return 12500 + (int)a; }
    };
    inline int takeU2(U2 u) { return 13000 + (int)u; }
    struct HoldA { A a = A::x; U2 u = w; };
    inline int u2OrLong(U2 u) { return 14000 + (int)u; }
    inline int u2OrLong(long long v) { return 14100 + (int)v; }
    // A free operator overloaded on int and on an enum class: an int operand binds the int one.
    struct OpFoo { int v = 1; };
    inline int operator+(const OpFoo&, int i) { return 15000 + i; }
    inline int operator+(const OpFoo&, A a) { return 15500 + (int)a; }
    // Enum LEFT operands of free operators: scoped and unscoped.
    inline int operator-(A a, const OpFoo&) { return 16000 + (int)a; }
    inline int operator*(U2 u, const OpFoo&) { return 16500 + (int)u; }
    constexpr A kA = A::x;

    // Free operators declared on an enum: an exact enum match beats built-in arithmetic, as in
    // clang. Each body returns a distinct value so the leg names the overload that ran.
    namespace eop
    {
        enum class SE : int { a = 5, b = 7 };
        enum UE : int { ua = 5, ub = 7 };
        enum class PrefixOnly : int { a = 5 };
        enum class PostfixOnly : int { a = 5 };
        enum Plain : int { p2 = 2, p3 = 3 };      // no overloads: built-in arithmetic only
        enum class PlainS : int { s2 = 2 };       // no overloads
        struct Unrel { int v = 1; };
        inline int operator+(const Unrel&, int i) { return 900 + i; }
        inline int operator-(int i, const Unrel&) { return 950 + i; }
        inline int operator+(SE e, int i) { return 20 + (int)e + i; }
        inline int operator+(int i, SE e) { return 40 + i + (int)e; }
        inline int operator+(SE e, double d) { return 300 + (int)e + (int)d; }
        inline int operator+(double d, SE e) { return 400 + (int)d + (int)e; }
        inline int operator*(SE x, SE y) { return 60 + (int)x * (int)y; }
        inline SE& operator++(SE& e) { e = (SE)((int)e + 10); return e; }
        inline SE operator++(SE& e, int) { SE old = e; e = (SE)((int)e + 20); return old; }
        inline SE& operator--(SE& e) { e = (SE)((int)e - 10); return e; }
        inline SE operator--(SE& e, int) { SE old = e; e = (SE)((int)e - 20); return old; }
        inline SE& operator+=(SE& e, int i) { e = (SE)((int)e + i * 10); return e; }
        inline SE& operator+=(SE& e, double d) { e = (SE)((int)e + (int)d * 30); return e; }
        inline SE& operator^(SE& e, int i) { e = (SE)((int)e + i * 20); return e; }
        inline bool operator<(SE x, SE y) { return (int)x > (int)y; }
        inline UE& operator++(UE& e) { e = (UE)((int)e + 10); return e; }
        inline UE operator++(UE& e, int) { UE old = e; e = (UE)((int)e + 20); return old; }
        inline UE& operator--(UE& e) { e = (UE)((int)e - 10); return e; }
        inline UE operator--(UE& e, int) { UE old = e; e = (UE)((int)e - 20); return old; }
        inline int operator+(UE e, int i) { return 80 + (int)e + i; }
        inline int operator-(int i, UE e) { return 100 + i - (int)e; }
        inline bool operator==(UE x, UE y) { return (int)x != (int)y; }
        inline PrefixOnly& operator++(PrefixOnly& e) { e = (PrefixOnly)((int)e + 10); return e; }
        inline PrefixOnly& operator--(PrefixOnly& e) { e = (PrefixOnly)((int)e - 10); return e; }
        inline PostfixOnly operator++(PostfixOnly& e, int) { PostfixOnly old = e; e = (PostfixOnly)((int)e + 20); return old; }
        inline PostfixOnly operator--(PostfixOnly& e, int) { PostfixOnly old = e; e = (PostfixOnly)((int)e - 20); return old; }
        struct Mem
        {
            int v = 3;
            int operator+(SE e) const { return 300 + v + (int)e; }
        };
        inline int operator+(const Mem& m, UE e) { return 400 + m.v + (int)e; }
        enum class Chain : int { first = 5 };
        inline Chain operator|(Chain e, int) { return (Chain)((int)e + 10); }
        inline int operator+(Chain e, int i) { return 100 + (int)e + i; }
        enum E : int { e0 = 2, e1 = 3, e2 = 5, e3 = 7 };
        enum N : int { n0 = 2 };
        inline int operator+(E e, int i) { return 800 + (int)e + i; }
        inline int operator+(N n, int i) { return 1000 + (int)n + i; }
        // Unary overload markers differ from the integral built-ins for both enum kinds.
        inline constexpr int operator~(SE e) { return 1500 + (int)e; }
        inline constexpr int operator-(SE e) { return 1600 + (int)e; }
        inline constexpr int operator+(SE e) { return 1700 + (int)e; }
        inline constexpr int operator!(SE e) { return 1800 + (int)e; }
        inline constexpr int operator~(UE e) { return 2000 + (int)e; }
        inline constexpr int operator-(UE e) { return 2100 + (int)e; }
        inline constexpr int operator+(UE e) { return 2200 + (int)e; }
        inline constexpr int operator!(UE e) { return 2300 + (int)e; }
        struct UnaryHolder { SE scoped; UE unscoped; };
        inline SE unarySE() { return SE::a; }
        enum class UnaryChain : int { first = 5 };
        inline constexpr UnaryChain operator~(UnaryChain e)
        { return (UnaryChain)((int)e + 10); }
        inline int operator+(UnaryChain e, int i) { return 100 + (int)e + i; }
        enum class GlobalUnary : int { value = 4 };
        enum OtherUnary : int { value2 = 5 };
    }
    namespace enumrelay
    {
        enum class GlobalAmbiguous : int { value = 5 };
        inline int operator+(GlobalAmbiguous, int) { return 11; }
        namespace EqualConversions
        {
            enum class E : int { value = 5 };
            inline int operator+(E, long) { return 21; }
            inline int operator+(E, unsigned long) { return 22; }
        }
        enum class Deleted : int { value = 5 };
        inline int operator+(Deleted, int) = delete;
        enum BuiltinLoses : int { builtinLoses = 5 };
        inline int operator+(int, BuiltinLoses) { return 207; }
        enum NoViable : int { noViable = 5 };
        inline int operator+(NoViable, const char*) { return 99; }
    }
}

inline int operator+(cppi_scope::enumrelay::GlobalAmbiguous, int) { return 12; }

// A global-namespace operator on a namespaced enum: found by ordinary lookup, not ADL.
inline int operator%(cppi_scope::eop::SE e, int i) { return 700 + (int)e + i; }
inline constexpr int operator+(cppi_scope::eop::GlobalUnary e) { return 4000 + (int)e; }
namespace cppi_scope::unrelated
{
    inline constexpr int operator~(eop::OtherUnary e) { return 9000 + (int)e; }
}
