// `explicit` on a C++ constructor, at a CFlat call argument. C++ offers exactly ONE implicit
// user-defined conversion at an argument and an `explicit` constructor is not a candidate for
// it; the spelled form `cppexp.Ex(5)` stays legal. The non-explicit `Options(double)` shapes
// below are the ACCEPT set: they must keep converting at every parameter kind C++ allows.
#pragma once
#include <type_traits>
#include <vector>

namespace cppexp {

struct Ex { int v; explicit Ex(int a) : v(a) {} };
inline int take_ex(Ex e) { return e.v; }
inline int take_ex_cref(const Ex& e) { return e.v; }
inline int take_ex_rref(Ex&& e) { return e.v; }
inline int take_ex_two(Ex e, int k) { return e.v + k; }

struct ExHost {
    int base;
    ExHost(int b) : base(b) {}
    int member_take(Ex e) const { return base + e.v; }
    static int static_take(Ex e) { return e.v * 10; }
};

// An explicit constructor whose SECOND parameter has a default: the one-argument call shape
// exists, and is still not an implicit conversion.
struct ExDef { int v; explicit ExDef(int a, int b = 7) : v(a + b) {} };
inline int take_exdef(ExDef e) { return e.v; }

// Both an explicit and a non-explicit constructor, of different arities.
struct Both {
    int v;
    explicit Both(int a) : v(a) {}
    Both(int a, int b) : v(a * 100 + b) {}
};
inline int take_both(Both b) { return b.v; }

// A free operator whose OPERAND would need the conversion.
inline int operator-(const Ex& a, const Ex& b) { return a.v - b.v; }

// A C++ CONSTRUCTOR parameter of class type.
struct ExOwner { int v; ExOwner(Ex e) : v(e.v + 1) {} };

// An explicit COPY constructor: copying an ExCopy must keep working.
struct ExCopy {
    int v;
    ExCopy(int a) : v(a) {}
    explicit ExCopy(const ExCopy& o) : v(o.v + 1000) {}
};
inline int take_excopy_cref(const ExCopy& c) { return c.v; }

// ACCEPT set - a NON-explicit converting constructor, offered at every parameter kind.
struct Options { double lr; Options(double l) : lr(l) {} };
inline int opt_by_value(Options o) { return (int)(o.lr * 100); }
inline int opt_by_cref(const Options& o) { return (int)(o.lr * 100) + 1; }
inline int opt_by_rref(Options&& o) { return (int)(o.lr * 100) + 2; }
inline int operator-(const Options& a, const Options& b) { return (int)((a.lr - b.lr) * 100); }
// A NON-const lvalue reference takes no user-defined conversion at all, free or member: the
// converted temporary has no address the callee could write through. A NAMED lvalue still binds.
inline int opt_by_lref(Options& o) { return (int)(o.lr * 100) + 3; }
struct OptHost {
    int base;
    OptHost(int b) : base(b) {}
    int member_lref(Options& o) const { return base + (int)(o.lr * 100); }
};
struct OptOwner { int v; OptOwner(Options o) : v((int)(o.lr * 100) + 7) {} };
struct Optim {
    int v;
    Optim(std::vector<int> params, Options o) : v((int)params.size() * 1000 + (int)(o.lr * 100)) {}
};
struct OptimCref {
    int v;
    OptimCref(std::vector<int> p, const Options& o) : v((int)p.size() * 2000 + (int)(o.lr * 100)) {}
};

struct NewPlain {
    int v;
    int how;
    NewPlain(int a, int b) : v(a + b), how(9) {}
};

struct NewTemplate {
    int v;
    int how;
    template<class T> NewTemplate(T x) : v((int)x + 20), how(1) {}
};

struct NewReq {
    int v;
    int how;
    template<class T> requires (sizeof(T) == 8)
    NewReq(T x) : v((int)x + 100), how(2) {}
};

struct NewEnabled {
    int v;
    int how;
    template<class T, std::enable_if_t<std::is_same_v<T, double>, int> = 0>
    NewEnabled(T x) : v((int)(x * 10.0)), how(3) {}
};

struct NewDefault {
    int v;
    int how;
    NewDefault() : v(41), how(4) {}
};

struct NewInner {
    int v;
    NewInner(int x) : v(x + 50) {}
};

struct NewWithMember {
    NewInner inner;
    int how;
    NewWithMember(int x) : inner(x), how(5) {}
};

struct NewDeleted {
    NewDeleted(int) = delete;
};

struct NewNarrow {
    int v;
    NewNarrow(int x) : v(x) {}
};
}
