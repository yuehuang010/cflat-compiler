#pragma once
#include <type_traits>
// T49: signed unscoped / CFlat enum arguments never bind a DIFFERENT scoped enum parameter (clang
// picks the int overload), enum cast operands convert through a converting-ctor template, and an
// optional-like class whose copy assignment is inherited (not listed) assigns an enum value.
namespace t49 {
enum class S { Scoped = 5 };
enum Signed : int { SignedVal = -3 };
enum Neg { N = -6 };
enum UnsignedE : unsigned { UV = 4 };
inline int only(S v) { return (int)v; }
inline int onlyref(const S& v) { return (int)v; }
inline int choose(S) { return 11; }
inline int choose(int) { return 10; }

template <class T> int deduceEnum(T) { return std::is_enum_v<T> ? 7 : 9; }
template <class T> int deduceEnumRef(T&) { return std::is_enum_v<T> ? 8 : 10; }
template <class T> int deduceEnumCref(const T&) { return std::is_enum_v<T> ? 9 : 11; }
inline int deduceEnumOverload(int) { return 10; }
template <class T> int deduceEnumOverload(T) { return std::is_enum_v<T> ? 11 : 12; }

template <class E> struct is_cond : std::false_type {};
enum class Code { bad = 22 };
template <> struct is_cond<Code> : std::true_type {};
struct Cond {
    int value_ = 0;
    Cond() = default;
    template <class E, typename std::enable_if<is_cond<E>::value, int>::type = 0>
    Cond(E e) : value_((int)e) {}
};
inline bool operator==(const Cond& a, const Cond& b) { return a.value_ == b.value_; }

template <class T> struct OptBase {
    T v{}; bool has = false;
    OptBase() = default;
    OptBase(const OptBase& o) : v(o.v), has(o.has) {}
    OptBase& operator=(const OptBase& o) { v = o.v; has = o.has; return *this; }
};
template <class T> struct Opt : OptBase<T> {
    Opt() = default;
    template <class U> Opt(U&& u) { this->v = (T)u; this->has = true; }
};
inline int bumps = 0;
inline int bump() { ++bumps; return 22; }
inline int charBump(char) { ++bumps; return 22; }
} // namespace t49

// T54: wrapped enum values keep their source identity through C++ macro generics.
namespace t54 {
enum Plain : int { PlainValue = 3 };
enum class Scoped : int { ScopedValue = 3 };
enum Byte : unsigned char { ByteValue = 3 };
enum class Other { OtherValue = 3 };
template <class T> struct accepted : std::false_type {};
template <> struct accepted<Plain> : std::true_type {};
template <> struct accepted<Scoped> : std::true_type {};
template <> struct accepted<Byte> : std::true_type {};
inline int ctorCalls = 0;
inline Plain plainValue() { return PlainValue; }
inline Scoped scopedValue() { return Scoped::ScopedValue; }
inline Byte byteValue() { return ByteValue; }
inline Plain plainFrom(int value) { return (Plain)value; }
inline Scoped scopedFrom(int value) { return (Scoped)value; }
inline Byte byteFrom(int value) { return (Byte)value; }
struct Sink {
    int value = 0;
    Sink() = default;
    template <class E, typename std::enable_if<accepted<E>::value, int>::type = 0>
    Sink(E e) : value((int)e + 100) { ++ctorCalls; }
};
struct PlainField { Plain value; };
struct ScopedField { Scoped value; };
struct ByteField { Byte value; };
inline Sink take(Sink value) { return value; }
// A plain (non-template) enum converting ctor on a trivially copyable class.
struct Sink2 {
    int value = 0;
    Sink2(Plain e) : value((int)e + 100) { ++ctorCalls; }
};
// One argument through a defaulted trailing parameter.
struct SinkDA {
    int value = 0;
    SinkDA(Plain e, int w = 7) : value((int)e + 100 + w) { ++ctorCalls; }
};
}
inline int t54_sink_value(t54::Sink value) { return value.value; }
#define T54_ID(x) (x)
#define T54_ID2(x, y) (x)
