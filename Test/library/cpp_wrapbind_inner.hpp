#pragma once
// WRAPBIND: free functions reached through another header's #include bind directly, whatever the
// include spelling (test_cpp_interop.cb imports wrapbind/cpp_wrapbind.hpp).
#define WRAPBIND_BASE 41
#define WRAPBIND_TWICE(x) ((x) * 2 + 1)
namespace cppwrapbind {
struct Obj {
    inline static int ctors = 0, copies = 0, moves = 0, dtors = 0, live = 0;
    inline static void reset() noexcept { ctors = copies = moves = dtors = 0; }
    inline static int sig() noexcept { return ctors * 1000 + copies * 100 + moves * 10 + dtors; }
    explicit Obj(int x) noexcept : v(x) { ++ctors; ++live; }
    Obj(const Obj& o) noexcept : v(o.v) { ++copies; ++live; }
    Obj(Obj&& o) noexcept : v(o.v) { o.v = -1; ++moves; ++live; }
    ~Obj() noexcept { ++dtors; --live; }
    const Obj& cref() const noexcept { return *this; }
    int v;
};
inline Obj mk(int x) noexcept { return Obj(x); }
inline int use(Obj o) noexcept { return o.v; }
inline int base() noexcept { return WRAPBIND_BASE + 1; }
}
