#pragma once
namespace w1demand {
template<class T> struct P {
    int f(const T& x) { return T::nope; }
    int f(long x) { return 9; }
};
template<class T> struct Q {
    T v;
    Q(const T& x) : v(x) { T::nope; }
    Q(double x) : v(11) {}
};
template<class T> struct Bad {
    static_assert(sizeof(T) == 3, "Bad copy");
    static const int value = 1;
};
template<class T> struct StaticValue {
    static inline int bad = Bad<T>::value;
    static inline int good = 8;
};
template<class T> struct M {
    int v = 2;
    M() {}
    M(const M& o) : v(o.v + Bad<T>::value) {}
};
struct ImplicitHolder { M<int> m; int x = 4; };
struct DefaultedHolder {
    M<int> m;
    int x = 4;
    DefaultedHolder() = default;
    DefaultedHolder(const DefaultedHolder&) = default;
};
}
namespace w11round3 {
template<class T> struct B {
    int v = 0;
    B(const T& x) { T::nope; }
    B(double x) : v(5) {}
};
inline int takeB(B<int> b) { return b.v; }
template<class T> struct C {
    int v = 0;
    C(const T& x) { T::nope; }
    template<class U> C(U&& x) : v(6) {}
};
inline int takeC(C<int> c) { return c.v; }
template<class T> struct Q {
    T v;
    Q(const T& x) : v(x) { T::nope; }
    Q(double x) : v(11) {}
};
// A non-template constructor wins the tie with a template: its failed body is the verdict.
template<class T> struct D {
    int v = 0;
    D(const T& x) { T::nope; }
    template<class U> D(U x, int extra = 0) : v(7) {}
};
template<class T> struct E {
    int v = 0;
    E(const T& x) { T::nope; }
    template<class U> E(U&& x) : v(8) {}
    E(double x) : v(9) {}
};
// Clang's own answer after CFlat probed Q(const int&) and never selected it: still constructible.
template<class T, class U> constexpr bool canMake = requires(U u) { T(u); };
inline int qIntConstructible() { return canMake<Q<int>, int> ? 1 : 0; }
// An inheriting constructor reaches the refused base body through its implicit base call.
template<class T> struct G : Q<T> { using Q<T>::Q; };
}
// MSVC's vector(size_type, const T&, const Alloc& = Alloc()) shape: the fill constructor's own
// body copies through a SFINAE-constrained construct_at, so clang reports "no matching function".
namespace w11fill {
struct Module { Module() = default; Module(const Module&) = delete; Module(Module&&) = default; };
struct KeyCheck : Module { int v; KeyCheck(int x) : v(x) {} };
struct Alloc {};
template<class U, class... Args>
auto construct_at(U* p, Args&&... args) -> decltype(U(static_cast<Args&&>(args)...), void()) {
    *p = U(static_cast<Args&&>(args)...);
}
template<class T> struct Fill {
    int n = 0;
    T* slot = nullptr;
    Fill(unsigned long count, const T& v, const Alloc& a = Alloc()) : n((int)count) {
        construct_at(slot, v);
    }
    Fill(unsigned long count) : n((int)count) {}
};
}
