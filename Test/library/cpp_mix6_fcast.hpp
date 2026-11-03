#pragma once
// C-style and functional casts of a scalar to an imported C++ class (MIX6 item 1): `(R)(7)` is
// `R(7)` by direct-initialization, so explicit constructors and every arithmetic source count.
// Values are checked against clang++ -std=c++20.
namespace cppfc {
struct R { int v; R(int x) : v(x * 3) {} };
struct RE { int v; explicit RE(int x) : v(x + 100) {} };
struct RD { int v; RD(double d) : v((int)(d * 2)) {} };
struct RB { int v; RB(bool b) : v(b ? 11 : 22) {} };
struct RBI { int v; RBI(bool b) : v(b ? 1 : 2) {} RBI(int x) : v(x + 50) {} };
inline int take_r(R r) { return r.v; }
}
// MIX10: a non-trivial class for `(R)b` with an unparenthesized scalar lvalue `b`; the live count
// checks that every object is destroyed.
namespace cppfc {
inline int rt_live_count = 0;
inline int rt_live() { return rt_live_count; }
struct RT {
    int v;
    explicit RT(int x) : v(x + 7) { ++rt_live_count; }
    RT(const RT& o) : v(o.v) { ++rt_live_count; }
    RT(RT&& o) noexcept : v(o.v) { o.v = -1; ++rt_live_count; }
    ~RT() { --rt_live_count; }
};
}
