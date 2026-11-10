#pragma once

// ARITHLD: C++ operators returning double or long double; long double keeps its identity in a chain.
namespace arithld
{
    struct LdCount
    {
        double v;
        static inline int born = 0, dead = 0;
        LdCount(double x = 0) : v(x) { ++born; }
        LdCount(const LdCount& o) : v(o.v) { ++born; }
        ~LdCount() { ++dead; }
        static void reset() { born = dead = 0; }
    };
    inline long double operator+(LdCount a, LdCount b) { return (long double)a.v + (long double)b.v; }
    inline double operator-(LdCount a, LdCount b) { return a.v - b.v; }
    inline int pick(double) { return 2; }
    inline int pick(long double) { return 3; }
    template<class T> int deduce(T x) { return pick(x); }
}
