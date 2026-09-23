#pragma once

namespace cppref {

struct Ex { int v; explicit Ex(int x) : v(x) {} };

inline int take_ref(Ex& e) { return ++e.v; }
inline int take_cref(const Ex& e) { return e.v + 1000; }
inline int take_rref(Ex&& e) { return e.v + 2000; }
inline int choose_cref(Ex& e) { return ++e.v + 100; }
inline int choose_cref(const Ex& e) { return e.v + 200; }
inline int choose_rref(Ex& e) { return ++e.v + 300; }
inline int choose_rref(Ex&& e) { return e.v + 400; }
inline Ex make_ex(int x) { return Ex(x); }
inline Ex operator+(const Ex& a, const Ex& b) { return Ex(a.v + b.v); }

inline Ex& ref_ex()
{
    static Ex value(70);
    return value;
}

struct RefOwner { int v; RefOwner(Ex& e) : v(++e.v) {} };

}
