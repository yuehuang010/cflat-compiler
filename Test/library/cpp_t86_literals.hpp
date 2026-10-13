#pragma once

namespace t86
{
    inline int pick(float) { return 1; }
    inline int pick(double) { return 2; }
    inline int pick(int) { return 3; }
    inline int pick(unsigned int) { return 4; }
    inline int pick(long) { return 5; }
    inline int pick(unsigned long) { return 6; }
    inline int pick(long long) { return 7; }
    inline int pick(unsigned long long) { return 8; }
    template<class T> int deduce(T x) { return pick(x); }
}
