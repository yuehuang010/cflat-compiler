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
class FpabiR2NT
{
public:
    static inline volatile int selectSecond = 0;
    inline explicit FpabiR2NT(int x) noexcept : v(x) {}
    inline FpabiR2NT(const FpabiR2NT& o) noexcept : v(o.v) {}
    inline ~FpabiR2NT() noexcept {}
    int v;
};

using FpabiR3ShortFn = short (*)(int);
using FpabiR3CharFn = char (*)(int);
using FpabiR3UCharFn = unsigned char (*)(int);
using FpabiR3BoolFn = bool (*)(int);
inline FpabiR3ShortFn fpabiR3SavedShort = nullptr;
inline int fpabiR3CallShort(FpabiR3ShortFn f, int x) { return (int)f(x); }
inline void fpabiR3StoreShort(FpabiR3ShortFn f) { fpabiR3SavedShort = f; }
inline int fpabiR3CallSavedShort(int x) { return (int)fpabiR3SavedShort(x); }
inline int fpabiR3ReturnedWhich = 0;
inline short fpabiR3CppShort(int x) { fpabiR3ReturnedWhich = 8; return (short)x; }
inline FpabiR3ShortFn fpabiR3GetShort() { return fpabiR3CppShort; }
inline int fpabiR3GetReturnedWhich() { return fpabiR3ReturnedWhich; }
inline int fpabiR3CallChar(FpabiR3CharFn f, int x) { return (int)f(x); }
inline int fpabiR3CallUChar(FpabiR3UCharFn f, int x) { return (int)f(x); }
inline int fpabiR3CallBool(FpabiR3BoolFn f, int x) { return f(x) ? 1 : 0; }

// By-value free result and by-value free parameter.
inline Cnt make(int x) noexcept { return Cnt(x); }
inline int readc(Cnt x) noexcept { return x.v; }
inline Cnt readc2(Cnt x) noexcept { return Cnt(x.v + 100); }
// By-value operands on a free operator.
inline Cnt operator+(Cnt a, Cnt b) noexcept { ++Cnt::plusCalls; return Cnt(a.v + b.v); }
class UnaryCnt
{
public:
    static inline int ctors = 0;
    static inline int copies = 0;
    static inline int moves = 0;
    static inline int dtors = 0;
    static inline int which = 0;
    static inline void reset() noexcept { ctors = 0; copies = 0; moves = 0; dtors = 0; which = 0; }
    inline UnaryCnt() noexcept : v(0) { ++ctors; }
    inline explicit UnaryCnt(int x) noexcept : v(x) { ++ctors; }
    inline UnaryCnt(const UnaryCnt& o) noexcept : v(o.v) { ++copies; }
    inline UnaryCnt(UnaryCnt&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
    inline UnaryCnt& operator=(const UnaryCnt& o) noexcept { v = o.v; return *this; }
    inline UnaryCnt& operator=(UnaryCnt&& o) noexcept { v = o.v; o.v = -1; return *this; }
    inline ~UnaryCnt() noexcept { v = -99; ++dtors; }
    inline UnaryCnt operator-() const noexcept { which = 1; return UnaryCnt(-v); }
    inline UnaryCnt operator+() const noexcept { which = 2; return UnaryCnt(v); }
    inline UnaryCnt operator!() const noexcept { which = 3; return UnaryCnt(v == 0 ? 1 : 0); }
    inline UnaryCnt operator~() const noexcept { which = 4; return UnaryCnt(~v); }
    inline UnaryCnt operator*() const noexcept { which = 5; return UnaryCnt(v + 100); }
    inline UnaryCnt method(int x) const noexcept { return UnaryCnt(v + x); }
    inline UnaryCnt operator*(const UnaryCnt& o) const noexcept { return UnaryCnt(v * o.v); }
    int v;
};
inline UnaryCnt unaryMake(int x) noexcept { return UnaryCnt(x); }
inline UnaryCnt unaryPick(UnaryCnt x) noexcept { return x; }
inline int readUnary(UnaryCnt x) noexcept { return x.v; }
class UnaryVV
{
public:
    inline UnaryVV() noexcept : e0(3), e1(4) {}
    inline UnaryCnt operator[](int i) const noexcept { return i ? e1 : e0; }
    UnaryCnt e0, e1;
};
class UnaryVR
{
public:
    inline UnaryVR() noexcept : e0(3), e1(4) {}
    inline UnaryCnt& operator[](int i) noexcept { return i ? e1 : e0; }
    UnaryCnt e0, e1;
};
class SubCnt
{
public:
    static inline int ctors = 0;
    static inline int copies = 0;
    static inline int moves = 0;
    static inline int dtors = 0;
    static inline void reset() noexcept { ctors = 0; copies = 0; moves = 0; dtors = 0; }
    inline SubCnt() noexcept : v(0) { ++ctors; }
    inline explicit SubCnt(int x) noexcept : v(x) { ++ctors; }
    inline SubCnt(const SubCnt& o) noexcept : v(o.v) { ++copies; }
    inline SubCnt(SubCnt&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
    inline ~SubCnt() noexcept { v = -99; ++dtors; }
    inline SubCnt operator*(const SubCnt& o) const noexcept { return SubCnt(v * o.v); }
    int v;
};
inline SubCnt operator+(SubCnt a, SubCnt b) noexcept { return SubCnt(a.v + b.v); }
class SubVV
{
public:
    inline SubVV() noexcept : e0(10), e1(11) {}
    inline SubCnt operator[](int i) const noexcept { return i ? e1 : e0; }
    SubCnt e0, e1;
};
class SubVR
{
public:
    inline SubVR() noexcept : e0(10), e1(11) {}
    inline SubCnt& operator[](int i) noexcept { return i ? e1 : e0; }
    SubCnt e0, e1;
};
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
    static inline int which = 0;
    static inline void reset() noexcept { ctors = 0; copies = 0; moves = 0; dtors = 0; which = 0; }
    inline CntA() noexcept : v(0) { ++ctors; }
    inline explicit CntA(int x) noexcept : v(x) { ++ctors; }
    inline CntA(const CntA& o) noexcept : v(o.v) { ++copies; }
    inline CntA(CntA&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
    inline CntA& operator=(const CntA& o) noexcept { v = o.v; return *this; }
    inline CntA& operator=(CntA&& o) noexcept { v = o.v; o.v = -1; return *this; }
    inline ~CntA() noexcept { v = -99; ++dtors; }
    inline int getRvalue() const noexcept { which = 2; return v; }
    int v;
};
inline int takeCntAByValue(CntA value) noexcept { CntA::which = 3; return value.v; }
inline int takeCntAByConstRef(const CntA& value) noexcept { CntA::which = 4; return value.v; }
class TernaryCnt
{
public:
    static inline int ctors = 0, copies = 0, moves = 0, dtors = 0;
    static inline int copyAssigns = 0, moveAssigns = 0;
    static inline void reset() noexcept { ctors = copies = moves = dtors = copyAssigns = moveAssigns = 0; }
    inline explicit TernaryCnt(int x = 0) noexcept : v(x) { ++ctors; }
    inline TernaryCnt(const TernaryCnt& o) noexcept : v(o.v) { ++copies; }
    inline TernaryCnt(TernaryCnt&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
    inline TernaryCnt& operator=(const TernaryCnt& o) noexcept { v = o.v; ++copyAssigns; return *this; }
    inline TernaryCnt& operator=(TernaryCnt&& o) noexcept { v = o.v; o.v = -1; ++moveAssigns; return *this; }
    inline ~TernaryCnt() noexcept { ++dtors; }
    int v;
};
class TernaryDerived : public TernaryCnt
{
public:
    inline explicit TernaryDerived(int x) noexcept : TernaryCnt(x) {}
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
