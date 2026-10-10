#pragma once
// T48: nullptr at C++ call boundaries - a nullptr VALUE keeps the decltype(nullptr) identity
// (overload picks, template deduction) and binds a class reference through C(std::nullptr_t).
#include <cstddef>
#include <type_traits>
namespace cpi_t48 {
inline int pa(int*) { return 11; }
inline int pa(std::nullptr_t) { return 12; }
inline int pb(void*) { return 21; }
inline int pb(std::nullptr_t) { return 22; }
inline int pc(bool) { return 31; }
inline int pc(std::nullptr_t) { return 32; }
template <class T> inline int pt(T) { return std::is_null_pointer<T>::value ? 7 : 8; }
inline int pe(int*) { return 51; }
inline int pn(std::nullptr_t) { return 61; }
inline int pg(std::nullptr_t&&) { return 71; }
inline int pg(void*) { return 72; }

struct N { int t; N(std::nullptr_t) : t(1) {} };
struct M { int t; M(int) : t(2) {} M(std::nullptr_t) : t(3) {} };
inline int rn(const N& n) { return 100 + n.t; }
inline int rvn(N&& n) { return 110 + n.t; }
inline int bvn(N n) { return 120 + n.t; }
inline int rm(const M& m) { return 200 + m.t; }
inline int bvm(M m) { return 210 + m.t; }
inline int q(const N&) { return 301; }
inline int q(int*) { return 302; }
struct H { int w(const N& n) const { return 400 + n.t; } static int s(const N& n) { return 410 + n.t; } };
struct L { int tag; L(std::nullptr_t&) : tag(4) {} };
inline int rl(const L& x) { return 700 + x.tag; }
inline int mx(const N&) { return 1; }
inline int mx(int*) { return 2; }
inline int lp(int* p) { return p == nullptr ? 5 : 6; }
inline int fp(void (*p)()) { return p == nullptr ? 3 : 4; }
inline int mf(const N&) { return 1; }
inline int mf(void (*)()) { return 2; }
inline int mpp(const N&) { return 3; }
inline int mpp(int**) { return 4; }
inline int md(const N&) { return 5; }
inline int md(int***) { return 6; }
inline int operator==(const N& a, const N& b) { return 500 + a.t + b.t; }
}
