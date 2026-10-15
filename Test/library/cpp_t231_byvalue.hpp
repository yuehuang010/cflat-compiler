#pragma once
// By-value C++ class parameters fed prvalues (T231): counted class, free, member, operator and
// derived-to-base shapes. Counts are checked against clang++ -std=c++20.

namespace cppbv {
class Cnt
{
public:
    static inline int ctors = 0;
    static inline int copies = 0;
    static inline int moves = 0;
    static inline int dtors = 0;
    static inline int plusCalls = 0;
    static inline void reset() noexcept { ctors = 0; copies = 0; moves = 0; dtors = 0; plusCalls = 0; }
    inline Cnt() noexcept : v(0) { ++ctors; }
    inline explicit Cnt(int x) noexcept : v(x) { ++ctors; }
    inline Cnt(const Cnt& o) noexcept : v(o.v) { ++copies; }
    inline Cnt(Cnt&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
    inline ~Cnt() noexcept { v = -99; ++dtors; }
    int v;
};
// By-value free result and by-value free parameter.
inline Cnt make(int x) noexcept { return Cnt(x); }
inline int readc(Cnt x) noexcept { return x.v; }
inline Cnt readc2(Cnt x) noexcept { return Cnt(x.v + 100); }
// By-value operands on a free operator.
inline Cnt operator+(Cnt a, Cnt b) noexcept { ++Cnt::plusCalls; return Cnt(a.v + b.v); }
// Reference-returning free operator (T246: a parenthesized lvalue result copies from it).
inline Cnt& operator<<(Cnt& a, int x) noexcept { a.v = a.v * 10 + x; return a; }
// Derived class whose base is the by-value parameter type.
class CntD : public Cnt
{
public:
    inline explicit CntD(int x) noexcept : Cnt(x), extra(0) {}
    int extra;
};
// T235 review: mixed overload set - by value with int, const& with double (clang picks per operand).
inline Cnt operator/(Cnt a, int b) noexcept { return Cnt(a.v / b); }
inline Cnt operator/(const Cnt& a, double b) noexcept { return Cnt(a.v * 1000); }
inline Cnt operator/(int b, Cnt a) noexcept { return Cnt(a.v / b); }
inline Cnt operator/(double b, const Cnt& a) noexcept { return Cnt(a.v * 2000); }
inline int takebase(Cnt x) noexcept { return x.v; }
// By-value member parameter.
struct Holder
{
    inline int take(Cnt x) const noexcept { return x.v; }
};
// Two by-value parameters (T235): a prvalue first, a direct construction second.
inline int pair(Cnt a, Cnt b) noexcept { return a.v * 10 + b.v; }
// Throws from the callee once both by-value parameters exist.
inline int pairThrow(Cnt a, Cnt b) { if (b.v > 100) throw a.v; return a.v * 10 + b.v; }
inline int boomArg(int v) { if (v > 0) throw v; return v; }
using CntFn = int (*)(int);
inline int guardInt(CntFn f, int x) { try { return f(x); } catch (int e) { return -e; } }
// T249: an assignable counted class (a lowered CFlat struct's C++ field): copy and move
// assignment move the value without counting, as clang's operator= does for this shape.
class CntA
{
public:
    static inline int ctors = 0;
    static inline int copies = 0;
    static inline int moves = 0;
    static inline int dtors = 0;
    static inline void reset() noexcept { ctors = 0; copies = 0; moves = 0; dtors = 0; }
    inline CntA() noexcept : v(0) { ++ctors; }
    inline explicit CntA(int x) noexcept : v(x) { ++ctors; }
    inline CntA(const CntA& o) noexcept : v(o.v) { ++copies; }
    inline CntA(CntA&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
    inline CntA& operator=(const CntA& o) noexcept { v = o.v; return *this; }
    inline CntA& operator=(CntA&& o) noexcept { v = o.v; o.v = -1; return *this; }
    inline ~CntA() noexcept { v = -99; ++dtors; }
    int v;
};
// T249: a member whose copy constructor may throw (an invoke splits the ternary arm block).
class TCopy
{
public:
    static inline int made = 0;
    static inline int dt = 0;
    inline TCopy() noexcept : v(0) { ++made; }
    inline explicit TCopy(int x) noexcept : v(x) { ++made; }
    inline TCopy(const TCopy& o) : v(o.v) { if (o.v < -1000) throw 7; ++made; }
    inline TCopy(TCopy&& o) noexcept : v(o.v) { o.v = -1; ++made; }
    inline TCopy& operator=(const TCopy& o) { v = o.v; return *this; }
    inline TCopy& operator=(TCopy&& o) noexcept { v = o.v; o.v = -1; return *this; }
    inline ~TCopy() noexcept { ++dt; }
    int v;
};
}
