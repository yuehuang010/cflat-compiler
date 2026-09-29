#pragma once
// Fixture for the free-operator-TEMPLATE binding path (return codes 2100-2149). libc++ declares
// every basic_string operator as a free function template over the class template; these classes
// reproduce that shape in a header the fixture controls, so the coverage is not std::string
// specific. Nt/NtFree hold the ACCEPT SET: non-template free operators, which must keep binding
// through the path that already handled them.
#include <compare>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string_view>
#include <type_traits>

namespace cppfop {

// A class TEMPLATE whose binary operators are FREE FUNCTION TEMPLATES. Each operator adds a
// distinct tag to its result so a leg can tell which overload C++ selected.
template <class T>
class Box {
public:
    Box() : v_(0) {}
    explicit Box(T v) : v_(v) {}
    T v() const { return v_; }
    T v_;
};

template <class T> Box<T> operator+(const Box<T>& a, const Box<T>& b)
{ return Box<T>(a.v_ + b.v_ + 1); }
template <class T> Box<T> operator+(const Box<T>& a, T b) { return Box<T>(a.v_ + b + 2); }
template <class T> Box<T> operator+(T a, const Box<T>& b) { return Box<T>(a + b.v_ + 3); }
template <class T> Box<T> operator-(const Box<T>& a, const Box<T>& b)
{ return Box<T>(a.v_ - b.v_ + 4); }
template <class T> Box<T>& operator|(Box<T>& a, const Box<T>& b)
{ a.v_ += b.v_; return a; }
template <class T> Box<T> operator^(Box<T>& a, const Box<T>& b)
{ a.v_ += b.v_; return a; }
template <class T> bool operator==(const Box<T>& a, const Box<T>& b) { return a.v_ == b.v_; }
template <class T> bool operator!=(const Box<T>& a, const Box<T>& b) { return a.v_ != b.v_; }
template <class T> bool operator<(const Box<T>& a, const Box<T>& b) { return a.v_ < b.v_; }
template <class T> bool operator>(const Box<T>& a, const Box<T>& b) { return a.v_ > b.v_; }
template <class T> bool operator<=(const Box<T>& a, const Box<T>& b) { return a.v_ <= b.v_; }
template <class T> bool operator>=(const Box<T>& a, const Box<T>& b) { return a.v_ >= b.v_; }

// A second class template that declares only == and <=> as free templates, the C++20 shape
// libc++ uses for basic_string: the four relational operators exist only as REWRITES.
template <class T>
class Ord {
public:
    Ord() : v_(0) {}
    explicit Ord(T v) : v_(v) {}
    T v() const { return v_; }
    T v_;
};

template <class T> bool operator==(const Ord<T>& a, const Ord<T>& b) { return a.v_ == b.v_; }
template <class T> std::strong_ordering operator<=>(const Ord<T>& a, const Ord<T>& b)
{ return a.v_ <=> b.v_; }

// ACCEPT SET: a plain class with NON-template free operators. These bound before the template
// path existed and must keep binding through the same candidate scan.
class Nt {
public:
    Nt() : n_(0) {}
    explicit Nt(int n) : n_(n) {}
    int n() const { return n_; }
    int n_;
};

inline Nt operator+(const Nt& a, const Nt& b) { return Nt(a.n_ + b.n_ + 10); }
inline bool operator==(const Nt& a, const Nt& b) { return a.n_ == b.n_; }
inline bool operator<(const Nt& a, const Nt& b) { return a.n_ < b.n_; }

}

namespace cppceval {
struct PtrText {
    const char* value;
    consteval PtrText(const char* s) : value(s) {}
};

struct ArrayText {
    const char* value;
    template<std::size_t N> consteval ArrayText(const char (&s)[N]) : value(s) {}
};

template<class T> int first(PtrText s, T) { return s.value[0]; }
template<class T> int later(T, PtrText s) { return s.value[0]; }
template<class T> int pair(PtrText a, PtrText b, T)
{ return (int)a.value[0] * 100 + (int)b.value[0]; }
template<class T> bool escaped_braces(PtrText s, T)
{ return s.value[0] == '{' && s.value[1] == '}' && s.value[2] == '\0'; }
template<class T> int array_first(ArrayText s, T) { return s.value[0]; }

inline int printed_value = 0;
template<class T> void print(PtrText s, T n) { printed_value = s.value[0] + (int)n; }
inline int printed() { return printed_value; }

struct Host {
    template<class T> int member(PtrText s, T) const { return s.value[0]; }
};
inline Host host{};
inline Host& object() { return host; }

template<class T> int forwarding_kind(T&&)
{ return std::is_same_v<std::decay_t<T>, const char*> ? 1 : 0; }
inline int plain_pointer(const char* s) { return (int)std::strlen(s); }
inline int string_view_size(std::string_view s) { return (int)s.size(); }

struct ByteText {
    const char* value;
    std::size_t size;
    template<std::size_t N> consteval ByteText(const char (&s)[N]) : value(s), size(N) {}
    int check(int id) const {
        switch (id) {
        case 0: { static constexpr char e[] = "A\"B"; return size == sizeof(e) && std::strlen(value) == 3 && std::memcmp(value, e, sizeof(e)) == 0; }
        case 1: { static constexpr char e[] = "A\\B"; return size == sizeof(e) && std::strlen(value) == 3 && std::memcmp(value, e, sizeof(e)) == 0; }
        case 2: { static constexpr char e[] = "A\nB"; return size == sizeof(e) && std::strlen(value) == 3 && std::memcmp(value, e, sizeof(e)) == 0; }
        case 3: { static constexpr char e[] = {'A', 0, 'B', 0}; return size == sizeof(e) && std::strlen(value) == 1 && std::memcmp(value, e, sizeof(e)) == 0; }
        case 4: { static constexpr char e[] = "??="; return size == sizeof(e) && std::strlen(value) == 3 && std::memcmp(value, e, sizeof(e)) == 0; }
        case 5: { static constexpr char e[] = {char(0xe9), 0}; return size == sizeof(e) && std::strlen(value) == 1 && std::memcmp(value, e, sizeof(e)) == 0; }
        case 6: { if (size != 1001 || std::strlen(value) != 1000) return 0; for (int i = 0; i < 1000; ++i) if (value[i] != 'A') return 0; return value[1000] == 0; }
        case 7: { static constexpr char e[] = "*/"; return size == sizeof(e) && std::strlen(value) == 2 && std::memcmp(value, e, sizeof(e)) == 0; }
        default: return 0;
        }
    }
};
template<class T> int check_bytes(ByteText s, T id) { return s.check((int)id); }

}

namespace cppfop {
// A class TEMPORARY operand of a free operator template is the caller's own object, never a
// bitwise re-spill of it: FopSelfRef counts a copy/move/dtor on an object not where it was built.
inline int selfref_built = 0, selfref_dead = 0, selfref_bad = 0;
struct FopSelfRef {
    FopSelfRef* self;
    int v;
    explicit FopSelfRef(int x) : self(this), v(x) { ++selfref_built; }
    FopSelfRef(const FopSelfRef& o) : self(this), v(o.v) { ++selfref_built; if (o.self != &o) ++selfref_bad; }
    FopSelfRef(FopSelfRef&& o) : self(this), v(o.v) { ++selfref_built; if (o.self != &o) ++selfref_bad; }
    ~FopSelfRef() { ++selfref_dead; if (self != this) ++selfref_bad; }
    int ok() const { return self == this ? v : -1000; }
};
inline bool selfref_clean() { return selfref_bad == 0 && selfref_built == selfref_dead; }
inline void selfref_reset() { selfref_built = selfref_dead = selfref_bad = 0; }
struct FopSink {};
template<class F> int operator+(const FopSink&, F f) { return f.ok(); }
template<class F> int operator-(const FopSink&, const F& f) { return f.ok(); }
template<class F> bool operator==(const FopSink&, F f) { return f.ok() == 7; }
template<class F> int operator%(const FopSink&, F f) { return f(40); }
struct FopCompound { int value; FopCompound() : value(0) {} };
template<class F> FopCompound& operator+=(FopCompound& o, F f) { o.value += f.ok(); return o; }

// Three-operand fold probes: non-trivial by-value results use sret; scalar results exercise
// the same fold handoff when the first overloaded operation returns an integer or bool.
struct FoldBox {
    int v;
    FoldBox(int x = 0) : v(x) {}
    ~FoldBox() {}
};
inline FoldBox operator+(const FoldBox& a, const FoldBox& b) { return FoldBox(a.v + b.v); }
inline FoldBox operator*(const FoldBox& a, const FoldBox& b) { return FoldBox(a.v * b.v); }
inline FoldBox operator|(const FoldBox& a, const FoldBox& b) { return FoldBox(a.v | b.v); }
inline FoldBox operator^(const FoldBox& a, const FoldBox& b) { return FoldBox(a.v ^ b.v); }
inline FoldBox operator&(const FoldBox& a, const FoldBox& b) { return FoldBox(a.v & b.v); }
inline FoldBox operator&&(const FoldBox& a, const FoldBox& b) { return FoldBox((a.v != 0) && (b.v != 0)); }
inline FoldBox operator||(const FoldBox& a, const FoldBox& b) { return FoldBox((a.v != 0) || (b.v != 0)); }

struct FoldScalar {
    int v;
    explicit FoldScalar(int x = 0) : v(x) {}
    operator bool() const { return v != 0; }
};
inline int operator+(FoldScalar a, FoldScalar b) { return a.v + b.v; }
inline int operator+(int a, FoldScalar b) { return a + b.v; }
inline int operator*(FoldScalar a, FoldScalar b) { return a.v * b.v; }
inline int operator*(int a, FoldScalar b) { return a * b.v; }
inline int operator|(FoldScalar a, FoldScalar b) { return a.v | b.v; }
inline int operator|(int a, FoldScalar b) { return a | b.v; }
inline int operator^(FoldScalar a, FoldScalar b) { return a.v ^ b.v; }
inline int operator^(int a, FoldScalar b) { return a ^ b.v; }
inline int operator&(FoldScalar a, FoldScalar b) { return a.v & b.v; }
inline int operator&(int a, FoldScalar b) { return a & b.v; }
inline bool operator&&(FoldScalar a, FoldScalar b) { return bool(a) && bool(b); }
inline bool operator||(FoldScalar a, FoldScalar b) { return bool(a) || bool(b); }
}

// Unary `- + ! ~` lookup on a class (return codes 31900-31929): free operators (plain, function
// templates, through a base), built-in operators reached through ONE implicit arithmetic
// conversion function, and the classes clang refuses. Every operator adds a distinct tag to its
// result so a leg can tell which overload C++ selected. The values are clang++ -std=c++20's.
namespace cppfopun {
struct R { int v = 0; R() = default; R(int x) : v(x) {} };

// Free operators, NOT inline: bound on first lookup in the class's namespace.
struct F { int v = 5; };
R operator-(const F& a) { return R(-a.v * 10 - 1); }
R operator+(const F& a) { return R(a.v * 10 + 2); }
int operator!(const F& a) { return a.v + 200; }
R operator~(const F& a) { return R(a.v * 10 + 4); }
// The same through a base class.
struct FB : F { };

// Free operator TEMPLATES over a class template.
template <class T> struct G { T v = 5; };
template <class T> R operator-(const G<T>& a) { return R((int)a.v * -10 - 2); }
template <class T> R operator+(const G<T>& a) { return R((int)a.v * 10 + 3); }
template <class T> int operator!(const G<T>& a) { return (int)a.v + 300; }
template <class T> R operator~(const G<T>& a) { return R((int)a.v * 10 + 5); }

// A free operator taking its operand by value, one taking it by mutable reference.
struct V { int v = 2; };
inline V operator-(V a) { return V{-a.v * 7}; }
struct MR { int v = 3; };
inline int operator~(MR& a) { a.v += 10; return a.v; }
// A free operator returning the operand's own type (chains).
struct FC { int v = 5; };
inline FC operator-(const FC& a) { return FC{-a.v}; }
// Nontrivial: live() counts constructed minus destroyed objects.
struct NT { static inline int live_ = 0; int v = 6;
    NT() { ++live_; } NT(const NT& o) : v(o.v) { ++live_; } ~NT() { --live_; } };
inline NT operator-(const NT& a) { NT r; r.v = -a.v; return r; }
inline NT mkNT() { NT r; r.v = 9; return r; }
inline int liveNT() { return NT::live_; }

// Member operators, own and inherited; a member operator that mutates the object.
struct M { int v = 5;
    R operator-() const { return R(-v * 10); }
    R operator+() const { return R(v * 10 + 1); }
    int operator!() const { return v + 100; }
    R operator~() const { return R(v * 10 + 3); } };
struct D : M { };
struct MM { int v = 3; int operator-() { v += 10; return v; } int operator~() { v += 20; return v; } };

// No unary operator at all, and conversions clang does not use for a built-in operator.
struct N { int v = 5; };
struct E { int v = 7; explicit operator int() const { return v; } };
struct EB { bool v = true; explicit operator bool() const { return v; } };
struct Two { int q = 0; operator int() const { return 1; } operator double() const { return 2.5; } };

// ONE implicit conversion function: C++ applies the built-in operator after it (with integral
// promotion for `- + ~`).
struct I { int v = 7; operator int() const { return v; } };
struct B { bool v = true; operator bool() const { return v; } };
struct S { short v = 5; operator short() const { return v; } };
struct Dbl { double v = 2.5; operator double() const { return v; } };
struct Fl { float v = 1.5f; operator float() const { return v; } };

// Read-only operands: a const global and a const& result must never be written by a non-const operator.
struct CM2 { int v = 3; int operator-() const { return v; } int operator-() { v += 10; return v; }
    int operator!() const { return 50; } int operator!() { v += 20; return 60; } };
inline const CM2 gcm2{};
inline const CM2& refcm2() { return gcm2; }
}
// Global namespace: a free operator with no namespace prefix.
struct FopunGlobal { int v = 4; };
inline FopunGlobal operator-(const FopunGlobal& a) { return FopunGlobal{-a.v * 3}; }
inline int operator!(const FopunGlobal& a) { return a.v + 1000; }
