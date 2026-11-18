// C++ fixture for class prvalue elements in a braced aggregate init (BRACEPRV): the element is
// built in place inside the aggregate, as clang does, never in a temporary moved in.
// Oracle: the same shapes in plain C++ under clang++ -std=c++20 (counts pinned by the legs).
#pragma once
namespace braceprv {
struct BS_Stats {
    inline static int ctor = 0;
    inline static int copy = 0;
    inline static int move = 0;
    inline static int dtor = 0;
    inline static int order = 0;   // decimal digits appended in construction/evaluation order
    static void reset() { ctor = copy = move = dtor = order = 0; }
    static int code() { return ctor * 1000 + copy * 100 + move * 10 + dtor; }
    static int note(int d) { order = order * 10 + d; return d; }
};

struct BS {
    int value;
    BS() : value(0) { ++BS_Stats::ctor; }
    BS(int x) : value(x) { ++BS_Stats::ctor; BS_Stats::note(x); }
    BS(int x, int y) : value(x * 10 + y) { ++BS_Stats::ctor; }
    BS(const BS& other) : value(other.value) { ++BS_Stats::copy; }
    BS(BS&& other) : value(other.value) { ++BS_Stats::move; }
    ~BS() { ++BS_Stats::dtor; }
};

struct BC_Stats {
    inline static int ctor = 0;
    inline static int copy = 0;
    inline static int move = 0;
    inline static int dtor = 0;
    static void reset() { ctor = copy = move = dtor = 0; }
    static int code() { return ctor * 1000 + copy * 100 + move * 10 + dtor; }
};
struct BC_T { int value; BC_T(int x) : value(x) {} };
struct BC_S {
    int value;
    BC_S(BC_T x) : value(x.value + 100) { ++BC_Stats::ctor; }
    BC_S(const BC_S& x) : value(x.value) { ++BC_Stats::copy; }
    BC_S(BC_S&& x) : value(x.value) { ++BC_Stats::move; }
    ~BC_S() { ++BC_Stats::dtor; }
};
inline BC_T bc_mkT(int x) { return BC_T(x); }

struct BSMaker {
    BSMaker() {}
    BS mk(int x) const { return BS(x); }
};
struct Reverse { int head; BS member; };
inline int callShared = 0;
inline BS mkWrite(int x) { callShared = x; BS_Stats::note(8); return BS(x); }
inline int readShared() { BS_Stats::note(9); return callShared; }
inline BS throwingMk(int x) { BS value(x); throw 7; }
inline int throwingRead() { throw 8; }

// Throws from its constructor when asked, after the earlier elements were built.
struct BT {
    int value;
    BT(int t) : value(t) { if (t) throw 1; }
};

struct Outer { BS r; int x; int y = 9; };
// Pointer-argument constructors (string literal, char*, function pointer), counted in BS_Stats.
struct BL {
    int value;
    BL(const char* s) : value(0) { while (s[value] != 0) ++value; ++BS_Stats::ctor; }
    BL(int (*f)(int)) : value(f(3)) { ++BS_Stats::ctor; }
    BL(const char8_t* s) : value(50) { while (s[value - 50] != 0) ++value; ++BS_Stats::ctor; }
    BL(const BL& other) : value(other.value) { ++BS_Stats::copy; }
    BL(BL&& other) : value(other.value) { ++BS_Stats::move; }
    ~BL() { ++BS_Stats::dtor; }
};
struct LAgg { BL l; int x; };
struct Multi { BS a; int x; BS b; int y; int z; };
struct Thrower { BS a; BT t; int x; };
// Private copy and move: only guaranteed elision builds it.
struct BP { int value; BP(int x) : value(x) {} ~BP() {} private: BP(const BP&); BP(BP&&); };
struct PAgg { BP p; int x; };
struct Wrap { Outer o; int z; };
struct Ref { const BS& r; int x; };

inline BS mk(int x) { return BS(x); }
inline int take(BS b) { return b.value; }
inline int gslot = 5;
inline int& slot() { return gslot; }
inline BS gbs(8);
inline BS& bsref() { return gbs; }
inline const BS& cbsref() { return gbs; }
struct BW { int value; BW(int& r) : value(r) { r = 77; } BW(const BS& s) : value(s.value) {} };
struct WAgg { BW w; int x; };
inline int code() { return BS_Stats::code(); }
typedef int (*Cb)(int);
inline int guarded(Cb f, int x) { try { return f(x); } catch (int) { return -1; } }
inline int guarded_thrower(int t)
{
    try { Thrower th = { BS(1), BT(t), 2 }; return th.a.value; } catch (int) { return -1; }
}
}
