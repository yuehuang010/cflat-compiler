#pragma once

namespace nttp
{
    enum class Choice { Low = 4, High = 9 };

    template<int N>
    inline int pick(const int* a) { return a[N]; }

    template<int N, class T>
    inline T scale(T v) { return static_cast<T>(v * N); }

    template<class T>
    inline T zero() { return static_cast<T>(sizeof(T)); }

    template<auto V>
    inline int val() { return static_cast<int>(V); }

    template<Choice V>
    inline int enumVal() { return static_cast<int>(V); }
}
