#pragma once
#include "cpp_t231_byvalue.hpp"

namespace cppbv {
inline TernaryCnt&& grv() noexcept { return ternaryGetRvalue(); }
inline TernaryCnt mk(int value) noexcept { return TernaryCnt(value); }
class TernaryPtrJoin
{
public:
    static inline int ctors = 0, copies = 0, moves = 0, dtors = 0, last = 0;
    static inline void reset() noexcept { ctors = copies = moves = dtors = last = 0; }
    inline explicit TernaryPtrJoin(int x) noexcept : v(x) { ++ctors; last = v; }
    inline TernaryPtrJoin(TernaryPtrJoin* p) noexcept : v(p != nullptr ? p->v : 0)
    { ++ctors; last = v; }
    inline TernaryPtrJoin(const TernaryPtrJoin& o) noexcept : v(o.v) { ++copies; }
    inline TernaryPtrJoin(TernaryPtrJoin&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
    inline ~TernaryPtrJoin() noexcept { ++dtors; }
    int v;
};
class TernaryPtrConversion
{
public:
    static inline int calls = 0;
    inline explicit TernaryPtrConversion(TernaryPtrJoin* value) noexcept : value_(value) {}
    inline operator TernaryPtrJoin*() noexcept { ++calls; return value_; }
private:
    TernaryPtrJoin* value_;
};
class TernaryConstPtrConversion
{
public:
    static inline int calls = 0;
    inline explicit TernaryConstPtrConversion(const TernaryPtrJoin* value) noexcept
        : value_(value) {}
    inline operator const TernaryPtrJoin*() noexcept { ++calls; return value_; }
private:
    const TernaryPtrJoin* value_;
};
class TernaryDerivedPtrJoin : public TernaryPtrJoin
{
public:
    inline explicit TernaryDerivedPtrJoin(int x) noexcept : TernaryPtrJoin(x) {}
};
class TernaryDerivedPtrConversion
{
public:
    static inline int calls = 0;
    inline explicit TernaryDerivedPtrConversion(TernaryPtrJoin* value) noexcept
        : value_(value) {}
    inline operator TernaryPtrJoin*() noexcept { ++calls; return value_; }
private:
    TernaryPtrJoin* value_;
};
inline int takeTernaryPtrJoin(TernaryPtrJoin value) noexcept { return value.v; }
}
