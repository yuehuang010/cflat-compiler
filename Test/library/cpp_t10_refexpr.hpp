#pragma once

namespace t10_refexpr {
struct X { int value; };

struct Expr {
    using value_type = int;
    int base;
    const int* scalar;
    __attribute__((noinline)) static int clobber()
    {
        volatile int words[128];
        for (int i = 0; i < 128; ++i) words[i] = i * 17;
        return words[0];
    }
    int eval() const { clobber(); return base + *scalar; }
};

inline Expr operator+(const X& x, const int& scalar)
{
    return Expr{x.value, &scalar};
}

// The scalar formal is NOT the result's value_type: by-value `double`, so `b - 2.5` keeps 2.5.
template <class T> struct Box { using value_type = T; T v{}; };
template <class T> Box<T> operator-(const Box<T>& b, double s)
{
    Box<T> r;
    r.v = (T)(b.v * 10 - s * 2);
    return r;
}
}
