#pragma once
// forward_as_tuple-shaped templates whose result refers to their arguments (t28): a string
// literal or function name forwarded through a generated wrapper must not leave the reference
// pointing into the wrapper's dead frame.
namespace t28 {
template<class T> struct Ref1 { T&& r; };
template<class T> Ref1<T> fwd1(T&& x) { return Ref1<T>{ static_cast<T&&>(x) }; }
template<class F> int call5(const Ref1<F>& r) { return r.r(5); }
template<class T> T& id_lref(T& x) { return x; }
// Overwrites the stack region a just-returned wrapper frame used.
inline int scribble() { volatile char buf[512]; for (int i = 0; i < 512; ++i) buf[i] = (char)0x5a; return buf[7]; }
inline int triple(int v) { return v * 3; }
}
// A std-namespace name takes the free-function wrapper path, as std::forward_as_tuple does.
namespace std { namespace cflat_t28 {
template<class... A> struct Tup;
template<class A> struct Tup<A> { A a; };
template<class A, class B> struct Tup<A, B> { A a; B b; };
template<class... A> constexpr Tup<A&&...> fwd(A&&... a) noexcept { return Tup<A&&...>{ static_cast<A&&>(a)... }; }
template<class T> int len(const T& t)
{
    volatile char pad[256];
    for (int i = 0; i < 256; ++i) pad[i] = 0x5a;
    const char* s = t.a;
    int n = 0;
    while (s[n]) ++n;
    return n;
}
template<class T> int call5(const T& t) { return t.a(5); }
// What std.cflat_t28.fwd("abcd") returns, as in C++: a reference to the literal's own array.
using LitRef = Tup<const char(&)[5]>;
inline int triple(int v) { return v * 3; }
// A literal is an array lvalue: the array-reference overload wins, `const char *&` is not viable.
inline int advanced = 0;
inline int advance(const char*& p) { advanced = 1; ++p; return 0; }
template<unsigned long N> int advance(const char (&)[N]) { advanced = 2; return (int)N - 1; }
inline int cref(const char* const& p) { int n = 0; while (p[n]) ++n; return n; }
// A literal's pointer temporary at `const char *const &` lives to the CALLER's full-expression.
__attribute__((noinline)) inline const char* const* capture(const char* const& p) { return &p; }
template<class T> __attribute__((noinline)) const char* const* capture_t(const char* const& p, const T&) { return &p; }
__attribute__((noinline)) inline int scribble() { volatile char b[2048]; for (int i = 0; i < 2048; ++i) b[i] = 0x5a; return b[0]; }
__attribute__((noinline)) inline int consume(const char* const* p, int) { int n = 0; while ((*p)[n]) ++n; return n; }
} }
// Direct (non-wrapper) binding: `T *const &` takes the literal's pointer, `T *&` refuses it.
namespace t28 {
inline int pick(const char*&) { return 1; }
inline int pick(const char* const&) { return 2; }
inline int retarget(char*& p) { static char other[] = "xy"; p = other; return 7; }
}
// Mixed overload sets: clang resolves over the literal's array; a winner binding the decayed
// pointer at `const char *const &` keeps it until the CALLER's full-expression ends.
namespace std { namespace cflat_t28 {
inline int chosen = 0;
inline const char* const* other() { static const char* q = "compete"; return &q; }
__attribute__((noinline)) inline const char* const* mixed_cref(const char* const& p) { chosen = 1; return &p; }
template<class T> const char* const* mixed_cref(const T&) { chosen = 2; return other(); }
__attribute__((noinline)) inline const char* const* mixed_fwd(const char* const& p) { chosen = 1; return &p; }
template<class T> const char* const* mixed_fwd(T&&) { chosen = 2; return other(); }
template<class U> __attribute__((noinline)) const char* const* arraywinner(const char* const& p, const U&) { chosen = 1; return &p; }
inline const char* const* arraywinner(const char (&)[4], const int&) { chosen = 2; return other(); }
} }
namespace t28 {
template<class U> __attribute__((noinline)) const char* const* mixed_t(const char* const& p, const U&) { std::cflat_t28::chosen = 1; return &p; }
template<class T, class U> const char* const* mixed_t(const T&, const U&) { std::cflat_t28::chosen = 2; return std::cflat_t28::other(); }
template<class U> __attribute__((noinline)) const char* const* mixed_tf(const char* const& p, const U&) { std::cflat_t28::chosen = 1; return &p; }
template<class T, class U> const char* const* mixed_tf(T&&, const U&) { std::cflat_t28::chosen = 2; return std::cflat_t28::other(); }
template<class U> __attribute__((noinline)) const char* const* arraywinner(const char* const& p, const U&) { std::cflat_t28::chosen = 1; return &p; }
inline const char* const* arraywinner(const char (&)[4], const int&) { std::cflat_t28::chosen = 2; return std::cflat_t28::other(); }
// Two literals, both decayed pointers retained by the result.
inline const char* const* second = nullptr;
__attribute__((noinline)) inline const char* const* two(const char* const& a, const char* const& b) { std::cflat_t28::chosen = 1; second = &b; return &a; }
template<class T, class V> const char* const* two(const T&, const V&) { std::cflat_t28::chosen = 2; second = std::cflat_t28::other(); return std::cflat_t28::other(); }
__attribute__((noinline)) inline int consume2(const char* const* p, int) { int a = 0, b = 0; while ((*p)[a]) ++a; while ((*second)[b]) ++b; return a * 10 + b; }
// Pointer-reference template vs array-reference template: ambiguous in clang.
template<class U> const char* const* amb(const char* const& p, const U&) { return &p; }
template<unsigned long N, class U> const char* const* amb(const char (&)[N], const U&) { return std::cflat_t28::other(); }
struct Mixed {
    template<class U> __attribute__((noinline)) const char* const* mt(const char* const& p, const U&) { std::cflat_t28::chosen = 1; return &p; }
    template<class T, class U> const char* const* mt(const T&, const U&) { std::cflat_t28::chosen = 2; return std::cflat_t28::other(); }
    template<class U> __attribute__((noinline)) const char* const* aw(const char* const& p, const U&) { std::cflat_t28::chosen = 1; return &p; }
    const char* const* aw(const char (&)[4], const int&) { std::cflat_t28::chosen = 2; return std::cflat_t28::other(); }
};
}
