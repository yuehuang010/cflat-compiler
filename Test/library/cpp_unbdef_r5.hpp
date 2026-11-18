#pragma once
namespace unbdef5
{
    struct C
    {
        int v;
        C() : v(0) {}
        C(int value) : v(value) {}
        C(const C& other) : v(other.v) {}
        ~C() {}
    };
    namespace
    {
        inline int fl(C& a, C b) { a.v = 9; return a.v * 10 + b.v; }
        inline int am(C a, int b = 3) { return 1; }
        inline int am(C a, double b = 4.0) { return 2; }
    }
}
