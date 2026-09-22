#pragma once
// Constructor TEMPLATES on user classes: plain, requires-constrained, enable_if, overloaded
// against a non-template, explicit, variadic, and on a nontrivial class. No standard header -
// enable_if is spelled locally. Each constructor records which overload ran in `how`.
namespace cppctor {
template <bool B, class T = void> struct enable_if_ {};
template <class T> struct enable_if_<true, T> { using type = T; };
template <class T> struct is_dbl { static constexpr bool value = false; };
template <> struct is_dbl<double> { static constexpr bool value = true; };

// Only a template constructor: `T(args)` must still be a constructor call.
struct Plain {
    int v;
    int how;
    template <class U> Plain(U u) : v((int)u * 2), how(1) {}
};
inline int take_plain(Plain p) { return p.v; }

// A long satisfies the constraint and is an exact match; Req(short) would narrow it.
struct Req {
    int v;
    int how;
    template <class U> requires (sizeof(U) == 8) Req(U u) : v((int)u + 100), how(2) {}
    Req(short s) : v(s), how(22) {}
};

struct Enab {
    int v;
    int how;
    template <class U, typename enable_if_<is_dbl<U>::value, int>::type = 0>
    Enab(U u) : v((int)(u * 10.0)), how(3) {}
    Enab(int a, int b) : v(a + b), how(33) {}
};
inline int take_enab(Enab e) { return e.v * 10 + e.how; }

// C++: the non-template wins an exact int; the template wins anything else.
struct Mixed {
    int v;
    int how;
    template <class U> Mixed(U u) : v((int)u), how(4) {}
    Mixed(int i) : v(i + 1000), how(44) {}
};

struct Expl {
    int v;
    int how;
    template <class U> explicit Expl(U u) : v((int)u + 7), how(5) {}
};
inline int take_expl(Expl e) { return e.v; }

struct Vari {
    int v;
    int how;
    template <class... A> Vari(A... a) : v((0 + ... + (int)a)), how(6) {}
};

// Nontrivial: `new Heavy(x)` takes the C++-allocator path.
struct Heavy {
    int v;
    int how;
    template <class U> requires (sizeof(U) == 8) Heavy(U u) : v((int)u * 3), how(7) { ++live_; }
    Heavy(short s) : v(s), how(77) { ++live_; }
    Heavy(const Heavy& o) : v(o.v), how(o.how) { ++live_; }
    ~Heavy() { --live_; }
    static int live() { return live_; }
    inline static int live_ = 0;
};
}
