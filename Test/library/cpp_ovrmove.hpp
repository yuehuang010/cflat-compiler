#pragma once

// By-value class parameters of C++ virtuals overridden in a CFlat [cpp] struct (OVRMOVE).
namespace ovm
{
    inline int moves = 0;
    inline int copies = 0;
    inline int dtors = 0;

    struct Mv
    {
        int v;
        Mv() : v(0) {}
        Mv(int x) : v(x) {}
        Mv(const Mv& o) : v(o.v + 1000) { ++copies; }
        Mv(Mv&& o) noexcept : v(o.v + 100) { o.v = -1; ++moves; }
        Mv& operator=(const Mv& o) { v = o.v + 1000; ++copies; return *this; }
        Mv& operator=(Mv&& o) noexcept { v = o.v + 100; o.v = -1; ++moves; return *this; }
        ~Mv() { ++dtors; }
    };

    // Trivial for calls: the template constructor is the move, the copy stays trivial.
    struct Tc
    {
        int v;
        Tc() : v(0) {}
        Tc(int x) : v(x) {}
        Tc(const Tc&) = default;
        template <class U> Tc(U&& u) : v(u.v + 100) { ++moves; }
    };

    struct Box { Mv m; int k; };
    // Callee-destroyed on every ABI: the wrapper still forwards a copy into the body.
    struct [[clang::trivial_abi]] Ta
    {
        int v;
        Ta() : v(0) {}
        Ta(int x) : v(x) {}
        Ta(const Ta& o) : v(o.v + 1000) { ++copies; }
        Ta(Ta&& o) noexcept : v(o.v + 100) { o.v = -1; ++moves; }
        ~Ta() { ++dtors; }
    };
    struct MvD : Mv { int e = 1; };
    struct Pod { int a; int b; };
    struct Triv
    {
        int v;
        Triv() : v(0) {}
        Triv(int x) : v(x) {}
    };
    struct TcBox { Tc t; int k; };

    class Base
    {
    public:
        virtual ~Base() = default;
        virtual int takeMv(Mv r) { return -1; }
        virtual int takeBox(Box r) { return -1; }
        virtual int takeTc(Tc r) { return -1; }
        virtual int takeTcBox(TcBox r) { return -1; }
        virtual int takeMvD(MvD r) { return -1; }
        virtual int takePod(Pod r) { return -1; }
        virtual int takeTriv(Triv r) { return -1; }
        virtual int takeTa(Ta r) { return -1; }
        virtual int writePod(Pod r) { return -1; }
        virtual int takeInt(int x) { return -1; }
        virtual int takePtr(int* p) { return -1; }
        virtual Mv giveMv(Mv r) { return r; }
    };

    inline void reset() { moves = 0; copies = 0; dtors = 0; }
    inline int get_moves() { return moves; }
    inline int get_copies() { return copies; }
    inline int get_dtors() { return dtors; }

    inline int call_mv_xvalue(Base* b, int v) { Mv x(v); return b->takeMv(static_cast<Mv&&>(x)); }
    inline int call_mv_prvalue(Base* b, int v) { return b->takeMv(Mv(v)); }
    inline int call_mv_lvalue(Base* b, int v) { Mv x(v); return b->takeMv(x); }
    inline int call_box_xvalue(Base* b, int v)
    {
        Box x; x.m.v = v; x.k = 2;
        return b->takeBox(static_cast<Box&&>(x));
    }
    inline int call_box_prvalue(Base* b, int v) { return b->takeBox(Box{ Mv(v), 2 }); }
    inline int call_tc_xvalue(Base* b, int v) { Tc x(v); return b->takeTc(static_cast<Tc&&>(x)); }
    inline int call_tc_lvalue(Base* b, int v) { Tc x(v); return b->takeTc(x); }
    inline int call_tcbox_xvalue(Base* b, int v)
    {
        TcBox x; x.t.v = v; x.k = 2;
        return b->takeTcBox(static_cast<TcBox&&>(x));
    }
    inline int call_mvd_xvalue(Base* b, int v) { MvD x; x.v = v; return b->takeMvD(static_cast<MvD&&>(x)); }
    inline int call_pod(Base* b, int v) { Pod x{ v, 2 }; return b->takePod(x); }
    inline int call_triv(Base* b, int v) { Triv x(v); return b->takeTriv(x); }
    inline int call_ta_xvalue(Base* b, int v) { Ta x(v); return b->takeTa(static_cast<Ta&&>(x)); }
    inline int call_write_pod(Base* b, int v)
    {
        Pod x{ v, 2 };
        int r = b->writePod(x);
        return r * 100 + x.a;
    }
    template <class T>
    class GBase
    {
    public:
        virtual ~GBase() = default;
        virtual T take(Mv r) { return T{}; }
    };
    template <class T>
    inline T call_generic(GBase<T>* b, int v) { Mv x(v); return b->take(static_cast<Mv&&>(x)); }
    template <class T>
    inline int call_method_mv(T* t, int v) { Mv x(v); return t->plain(static_cast<Mv&&>(x)); }
    inline int call_int(Base* b, int v) { return b->takeInt(v); }
    inline int call_ptr(Base* b, int v) { return b->takePtr(&v); }
    inline int call_give(Base* b, int v) { Mv r = b->giveMv(Mv(v)); return r.v; }
}
