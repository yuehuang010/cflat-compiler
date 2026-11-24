#pragma once
// OBJOIN: imported class with counting specials for owned-vs-place `?:` join legs (test_cpp_interop.cb).
namespace cppobjoin {
struct Obj {
    inline static int ctors = 0, copies = 0, moves = 0, cpasg = 0, mvasg = 0, dtors = 0, live = 0;
    inline static void reset() noexcept { ctors = copies = moves = cpasg = mvasg = dtors = 0; }
    inline static int sig() noexcept {
        return ctors * 100000 + copies * 10000 + moves * 1000 + (cpasg + mvasg * 10) * 10 + dtors;
    }
    Obj() noexcept : v(7) { ++ctors; ++live; }
    explicit Obj(int x) noexcept : v(x) { ++ctors; ++live; }
    Obj(const Obj& o) noexcept : v(o.v) { ++copies; ++live; }
    Obj(Obj&& o) noexcept : v(o.v) { o.v = -1; ++moves; ++live; }
    Obj& operator=(const Obj& o) noexcept { v = o.v; ++cpasg; return *this; }
    Obj& operator=(Obj&& o) noexcept { v = o.v; o.v = -1; ++mvasg; return *this; }
    ~Obj() noexcept { ++dtors; --live; }
    Obj operator+(Obj rhs) const noexcept { return Obj(v * 10 + rhs.v); }
    int v;
};
inline Obj mk(int x) noexcept { return Obj(x); }
inline int use(Obj o) noexcept { return o.v; }
inline Obj& ref(Obj& o) noexcept { return o; }
struct Holder { Obj o; int value() const noexcept { return o.v; } };
struct CHolder {
    explicit CHolder(Obj x) noexcept : o(static_cast<Obj&&>(x)) {}
    int value() const noexcept { return o.v; }
    Obj o;
};
}
