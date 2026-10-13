#pragma once

// T100: an l/L floating literal is long double; double and long double are distinct overloads.
namespace t100
{
    inline int pick(float) { return 1; }
    inline int pick(double) { return 2; }
    inline int pick(long double) { return 3; }
    template<class T> int deduce(T x) { return pick(x); }
    template<class T> int size(T) { return (int)sizeof(T); }
}
