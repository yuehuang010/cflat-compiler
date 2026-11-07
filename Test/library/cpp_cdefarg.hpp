#pragma once
#include <cstring>

// CDEFARG: omitted by-value class defaults are built in their parameter slots after the
// explicit arguments and destroyed at the end of the full expression, as clang does.
namespace cppcdef
{
    inline char trace[256] = {};
    inline int trace_size = 0;
    inline int live = 0;
    inline int seen_live = 0;

    inline void mark(char kind, int value)
    {
        if (trace_size < 254)
        {
            trace[trace_size++] = kind;
            trace[trace_size++] = (char)('0' + value % 10);
            trace[trace_size] = 0;
        }
    }
    inline void reset_trace() { trace_size = 0; trace[0] = 0; }
    inline int live_count() { return live; }
    inline bool trace_is(const char* expected) { return std::strcmp(trace, expected) == 0; }

    struct B
    {
        int value;
        B(int v = 20) : value(v) { mark('b', v); ++live; }
        B(const B& o) : value(o.value) { mark('c', value); ++live; }
        B(B&& o) : value(o.value) { mark('m', value); ++live; }
        ~B() { mark('y', value); --live; value = -7; }
    };
    inline B mk(int v) { return B(v); }
    inline B global_b = B(8);
    inline int enter(int v) { seen_live = live; mark('[', 0); return v; }

    inline int cdefB(B b, B d = B(5)) { return enter(b.value * 1000 + d.value); }
    inline int cdefFn(B b, B d = mk(7)) { return enter(b.value * 1000 + d.value); }
    inline int cdefCopy(B b, B d = global_b) { return enter(b.value * 1000 + d.value); }
    inline int cdefTwo(B b, B d = B(5), B e = B(6)) { return enter(b.value * 1000 + d.value * 10 + e.value); }
    inline int cdefAfter(B b, int e = 3, B d = B(5)) { return enter(b.value * 1000 + d.value * 10 + e); }
    struct H
    {
        int k = 0;
        static B seed() { return B(9); }
        int take(B b, B d = B(5)) { return enter(b.value * 1000 + d.value); }
        int takeS(B b, B d = seed()) { return enter(b.value * 1000 + d.value); }
    };
    // R1: the default calls an owner static named like a plain helper would be.
    struct M
    {
        int k = 0;
        static B make() { return B(4); }
        int take(B b, B d = make()) { return enter(b.value * 1000 + d.value); }
    };
    // A temporary nested in the default must live through the call: kept on clang's wrapper.
    struct Vw
    {
        const B* p;
        Vw(const B& b) : p(&b) {}
        ~Vw() {}
        int get() const { return p->value; }
    };
    inline int cdefNest(B b, Vw v = Vw(B(31))) { return enter(b.value * 1000 + v.get()); }
    struct K
    {
        int value;
        K(B b, B d = B(5)) : value(b.value * 1000 + d.value) { seen_live = live; mark('[', 0); }
    };
}

// A re-read default must bind what the declaration bound: later overloads (qualified, global)
// and a later using-directive must not change it (verified initializer, else the wrapper).
namespace cdefns { inline cppcdef::B pk(long v) { return cppcdef::B((int)v); } }
inline cppcdef::B cdefgp(long v) { return cppcdef::B((int)v); }
namespace cppcdef
{
    inline int cdefQual(B b, B d = cdefns::pk(1)) { return enter(b.value * 1000 + d.value); }
    inline B cdefgq(long v) { return B((int)v); }
    inline int cdefDir(B b, B d = cdefgq(1)) { return enter(b.value * 1000 + d.value); }
    inline int cdefGlob(B b, B d = ::cdefgp(1)) { return enter(b.value * 1000 + d.value); }
}
namespace cdefns { inline cppcdef::B pk(int v) { return cppcdef::B(v + 50); } }
namespace cdefum { inline cppcdef::B cdefgq(int v) { return cppcdef::B(v + 60); } }
namespace cppcdef { using namespace cdefum; }
inline cppcdef::B cdefgp(int v) { return cppcdef::B(v + 70); }
// Float/double defaults from integer constants round once, like clang.
namespace ff_cppdefaults
{
    inline float round_float(float value = 0x1000001000000001) { return value; }
    inline float negative_float(float value = -0x1000001000000001) { return value; }
    inline float unsigned_float(float value = 0x8000008000000001ULL) { return value; }
    inline double round_double(double value = 0x1000001000000001) { return value; }
}
