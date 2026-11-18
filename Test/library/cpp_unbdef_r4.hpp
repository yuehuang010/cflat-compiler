#pragma once
namespace unbdef4
{
    inline int& cc() { static int value = 0; return value; }
    inline int& cp() { static int value = 0; return value; }
    inline int& dc() { static int value = 0; return value; }
    inline void reset() { cc() = 0; cp() = 0; dc() = 0; }
    inline int stats() { return cc() * 100 + cp() * 10 + dc(); }
    struct C
    {
        int v;
        C() : v(0) { ++cc(); }
        C(int x) : v(x) { ++cc(); }
        C(const C& other) : v(other.v) { ++cp(); }
        ~C() { ++dc(); }
    };
    struct W { int v; W() : v(1) {} template <class U> W(U) : v(7) {} };
    struct ND { int v; ND(int x) : v(x) {} };
    struct EX { int v; explicit EX() : v(3) {} };
    namespace
    {
        inline int rr(C a, C&& b) { return a.v * 10 + b.v; }
        inline int lr(C a, C& b) { return a.v * 10 + b.v; }
        inline int va(int a, ...) { return a; }
        inline int o1(int a, int b) { return 100 + b; }
        inline int o1(int a, C b) { return 200 + b.v; }
        inline int o2(int a, double b) { return 1; }
        inline int o2(int a, int b) { return 2; }
        inline int tw(int a, W b) { return a * 10 + b.v; }
        inline int nd(int a, ND b) { return a * 10 + b.v; }
        inline int ex(int a, EX b) { return a * 10 + b.v; }
        inline int pt(int a, int* p) { return a * 10 + (p == nullptr ? 1 : 2); }
        inline int db(int a, double b) { return a * 10 + (int)(b + 1.5); }
        inline int fa(int a, auto b) { return a * 10 + (int)b; }
    }
}
namespace unbdef4bound
{
    inline int rr(unbdef4::C a, unbdef4::C&& b) { return a.v * 10 + b.v; }
    inline int nd(int a, unbdef4::ND b) { return a * 10 + b.v; }
    inline int tw(int a, unbdef4::W b) { return a * 10 + b.v; }
    inline int ex(int a, unbdef4::EX b) { return a * 10 + b.v; }
    inline int pt(int a, int* p) { return a * 10 + (p == nullptr ? 1 : 2); }
    inline int db(int a, double b) { return a * 10 + (int)(b + 1.5); }
    inline int o1(int a, int b) { return 100 + b; }
    inline int o1(int a, unbdef4::C b) { return 200 + b.v; }
    inline int o2(int a, double b) { return 1; }
    inline int o2(int a, int b) { return 2; }
}
