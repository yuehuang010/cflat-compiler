// Definitions for cpp_interop_basic.h. Compiled by the host C++ driver and linked into
// the final image; the CFlat side never parses this file.
#include "cpp_interop_basic.h"

namespace cppi
{
    int add(int a, int b) noexcept { return a + b; }
    int add(double a, double b) noexcept { return (int)(a + b) + 1000; }

    int pick_ref(int& x) noexcept { int old = x; x = 7; return old; }
    int read_cref(const int& x) noexcept { return x + 1; }

    int* ptr_ident(int* p) noexcept { return p; }

    static int g_slot = 99;
    int& ref_slot() noexcept { return g_slot; }

    long long widen(int v) noexcept { return (long long)v * 1000000000LL; }
    unsigned char uc(unsigned char v) noexcept { return (unsigned char)(v + 1); }

    int mode_value(Mode m) noexcept { return (int)m * 10; }

    namespace inner
    {
        int twice(int v) noexcept { return v * 2; }
    }

    int may_throw(int v) { return v; }

    int read_pair(const Pair* p) noexcept { return p->a + p->b; }

    Small small_bump(Small v) noexcept
    {
        Small r; r.a = (signed char)(v.a + 1); r.b = (unsigned char)(v.b + 1); return r;
    }
    Mixed mixed_bump(Mixed v) noexcept
    {
        Mixed r; r.d = v.d + 1.0; r.i = v.i + 1; return r;
    }
    Large large_bump(Large v) noexcept
    {
        Large r; r.x = v.x + 1; r.y = v.y + 1; r.z = v.z + 1; return r;
    }
    Hfa hfa_bump(Hfa v) noexcept
    {
        Hfa r; r.a = v.a + 1.0f; r.b = v.b + 1.0f; r.c = v.c + 1.0f; r.d = v.d + 1.0f; return r;
    }
    Packed packed_bump(Packed v) noexcept
    {
        Packed r; r.c = (unsigned char)(v.c + 1); r.n = v.n + 1; return r;
    }
    Aligned aligned_bump(Aligned v) noexcept
    {
        Aligned r; r.x = v.x + 1; r.y = v.y + 1; return r;
    }

    long long mixed_args(Small s, int k, Large l) noexcept
    {
        return (long long)s.a * 1000000 + (long long)s.b * 10000 + (long long)k * 100
             + (l.x + l.y + l.z);
    }

    int pair_and_small(const Pair* p, Small s) noexcept
    {
        return p->a + p->b + s.a + s.b;
    }

    unsigned long long size_of(int which) noexcept
    {
        switch (which)
        {
        case 0: return sizeof(Small);
        case 1: return sizeof(Mixed);
        case 2: return sizeof(Large);
        case 3: return sizeof(Hfa);
        case 4: return sizeof(Packed);
        case 5: return sizeof(Aligned);
        case 6: return sizeof(Pair);
        }
        return 0;
    }
    unsigned long long align_of(int which) noexcept
    {
        switch (which)
        {
        case 0: return alignof(Small);
        case 1: return alignof(Mixed);
        case 2: return alignof(Large);
        case 3: return alignof(Hfa);
        case 4: return alignof(Packed);
        case 5: return alignof(Aligned);
        case 6: return alignof(Pair);
        }
        return 0;
    }

    int Point::s_calls = 0;

    int  Point::sum() const noexcept          { return x + y; }
    void Point::bump(int d) noexcept           { x += d; y += d; ++s_calls; }
    int  Point::scaled(int f, int off) noexcept { return (x + y) * f + off; }
    int  Point::probe() noexcept               { return 1; }
    int  Point::probe() const noexcept         { return 2; }
    int  Point::origin_sum() noexcept          { return 0; }
    int  Point::hidden() const noexcept        { return hidden_; }

    // ---- M4b: nontrivial class lifetime instrumentation -------------------------
    static int g_ctor = 0;
    static int g_copy = 0;
    static int g_move = 0;
    static int g_dtor = 0;
    static int g_copy_assign = 0;
    static int g_move_assign = 0;

    Tracked::Tracked(int payload) noexcept : payload_(payload) { ++g_ctor; }
    Tracked::Tracked(const Tracked& other) noexcept : payload_(other.payload_) { ++g_copy; }
    Tracked::Tracked(Tracked&& other) noexcept : payload_(other.payload_) { other.payload_ = -1; ++g_move; }
    Tracked::~Tracked() noexcept { ++g_dtor; }
    Tracked& Tracked::operator=(const Tracked& other) noexcept
    {
        payload_ = other.payload_; ++g_copy_assign; return *this;
    }
    Tracked& Tracked::operator=(Tracked&& other) noexcept
    {
        payload_ = other.payload_; other.payload_ = -1; ++g_move_assign; return *this;
    }
    int Tracked::value() const noexcept { return payload_; }
    bool Tracked::operator==(const Tracked& other) const noexcept { return payload_ == other.payload_; }

    void reset_counts() noexcept
    {
        g_ctor = 0; g_copy = 0; g_move = 0; g_dtor = 0; g_copy_assign = 0; g_move_assign = 0;
    }
    int ctor_count() noexcept        { return g_ctor; }
    int copy_count() noexcept        { return g_copy; }
    int move_count() noexcept        { return g_move; }
    int dtor_count() noexcept        { return g_dtor; }
    int copy_assign_count() noexcept { return g_copy_assign; }
    int move_assign_count() noexcept { return g_move_assign; }

    NoCopy::NoCopy(int v) noexcept : v_(v) {}
    NoCopy::NoCopy(NoCopy&& other) noexcept : v_(other.v_) { other.v_ = -1; }
    NoCopy::~NoCopy() noexcept {}
    int NoCopy::value() const noexcept { return v_; }

    Tracked make_tracked(int payload) noexcept { return Tracked(payload); }
    int take_tracked(Tracked t) noexcept { return t.value(); }
    int take_moved(Tracked t) noexcept { return t.value() * 10; }
}

extern "C" int cppi_c_linkage(int v) noexcept { return v + 5; }
