#pragma once
#include <cstdlib>

namespace t242vj {
inline int liveCount = 0;
inline int newCount = 0;
inline int deleteCount = 0;
inline int dtorCount = 0;
inline int voidHits = 0;
inline int typedHits = 0;
inline void* lastValue = nullptr;

struct E
{
    int v;
    E(int value) : v(value) { ++liveCount; }
    ~E() { --liveCount; ++dtorCount; }
    static void* operator new(std::size_t size)
    { ++newCount; return std::malloc(size); }
    static void operator delete(void* value)
    { ++deleteCount; std::free(value); }
};

struct F {};

struct Ovv
{
    int r;
    Ovv(void* value, int) : r(-5) { ++voidHits; lastValue = value; }
    Ovv(E* value, int) : r(value == nullptr ? -1 : 1000 + value->v)
    { ++typedHits; lastValue = value; }
};

struct Ov
{
    int r;
    Ov(E*, int) : r(1) {}
    Ov(F*, int) : r(2) {}
};

inline void reset()
{
    liveCount = newCount = deleteCount = dtorCount = voidHits = typedHits = 0;
    lastValue = nullptr;
}
inline int live() { return liveCount; }
inline int news() { return newCount; }
inline int deletes() { return deleteCount; }
inline int dtors() { return dtorCount; }
inline int voidCalls() { return voidHits; }
inline int typedCalls() { return typedHits; }
inline void* lastArg() { return lastValue; }
}
