#pragma once
#include <type_traits>
#include <functional>
#include <memory>
#include <vector>
#include <optional>
#include <tuple>
// forward_as_tuple-shaped templates whose result refers to their arguments (t28): a string
// literal or function name forwarded through a generated wrapper must not leave the reference
// pointing into the wrapper's dead frame.
namespace t28 {
template<class T> struct Ref1 { T&& r; };
template<class T> Ref1<T> fwd1(T&& x) { return Ref1<T>{ static_cast<T&&>(x) }; }
template<class F> int call5(const Ref1<F>& r) { return r.r(5); }
template<class F> int call5plain(F&& f) { return f(5); }
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
// 1 when F deduces an lvalue reference, 2 for an rvalue or xvalue argument.
template<class T> struct IsLref { static constexpr int v = 2; };
template<class T> struct IsLref<T&> { static constexpr int v = 1; };
template<class F> int category(F&&) { return IsLref<F>::v; }
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
template<class F> struct ClosureHolder { F f; int run(int x) { return f(x); } };
template<class F> ClosureHolder<F> hold(F f) { return ClosureHolder<F>{ f }; }
template<class F> ClosureHolder<F> holdfwd(F&& f) { return ClosureHolder<F>{ static_cast<F&&>(f) }; }
template<class F> struct PointerHolder { F* f; int run(int x) { return (*f)(x); } };
template<class F> PointerHolder<std::remove_reference_t<F>> holdptr(F&& f) { return { &f }; }
template<class F> struct ConstRefHolder { const F& f; int run(int x) const { return f(x); } };
template<class F> ConstRefHolder<std::remove_reference_t<F>> holdcref(F&& f) { return { f }; }
// T419: results projected as opaque blobs (no public field) still reveal a held F* / F&.
template<class F> class PrivPtr { F* p; public: PrivPtr(F* q) : p(q) {} int run(int x) { return (*p)(x); } };
template<class F> PrivPtr<std::remove_reference_t<F>> holdpriv(F&& f) { return PrivPtr<std::remove_reference_t<F>>(&f); }
template<class F> class PrivNest { PrivPtr<F> in; public: PrivNest(F* q) : in(q) {} int run(int x) { return in.run(x); } };
template<class F> PrivNest<std::remove_reference_t<F>> holdnest(F&& f) { return PrivNest<std::remove_reference_t<F>>(&f); }
template<class F> std::tuple<std::remove_reference_t<F>&> holdtup(F&& f) { return std::tuple<std::remove_reference_t<F>&>(f); }
template<class F> std::optional<std::reference_wrapper<std::remove_reference_t<F>>> holdoptrw(F&& f) { return std::ref(f); }
// Blob controls: the closure by value, or an unrelated pointer - nothing refers to the argument.
template<class F> class PrivCopy { int* p; F f; public: PrivCopy(F g) : p(nullptr), f(g) {} int run(int x) { return p ? 0 : f(x); } };
template<class F> PrivCopy<std::remove_reference_t<F>> holdprivcopy(F&& f) { return PrivCopy<std::remove_reference_t<F>>(f); }
template<class F> std::tuple<std::remove_reference_t<F>, int*> holdtupcopy(F&& f) { return std::tuple<std::remove_reference_t<F>, int*>(f, nullptr); }
template<class F> std::optional<std::remove_reference_t<F>> holdoptcopy(F&& f) { return std::optional<std::remove_reference_t<F>>(f); }
// A borrowed F* deep in the layout, behind a wide (4095-record) sibling, or behind 64 unrelated pointers.
template<int N, class F> class DeepHold { DeepHold<N - 1, F> in; public: DeepHold(F& f) : in(f) {} int run(int x) { return in.run(x); } };
template<class F> class DeepHold<0, F> { F* p; public: DeepHold(F& f) : p(&f) {} int run(int x) { return (*p)(x); } };
template<class F> DeepHold<14, std::remove_reference_t<F>> holddeep(F&& f) { return { f }; }
template<int N> struct FillBlock { FillBlock<N - 1> a, b; };
template<> struct FillBlock<0> { char x = 0; };
template<class F> class WideHold { FillBlock<11> fill; DeepHold<0, F> tail; public: WideHold(F& f) : tail(f) {} int run(int x) { return tail.run(x); } };
template<class F> WideHold<std::remove_reference_t<F>> holdwide(F&& f) { return { f }; }
template<int N> struct SlotTag {};
template<class F, class Seq> class Crowd;
template<class F, int... I> class Crowd<F, std::integer_sequence<int, I...>> { std::tuple<SlotTag<I>*...> slots; F* tail; public: Crowd(F& f) : slots(), tail(&f) {} int run(int x) { return (*tail)(x); } };
template<class F> Crowd<std::remove_reference_t<F>, std::make_integer_sequence<int, 64>> holdcrowd(F&& f) { return { f }; }
// Owning containers point at their OWN copy; a pointer to Tag<F*> is not a pointer to F.
template<class F> std::vector<std::remove_reference_t<F>> holdvec(F&& f) { return { f }; }
template<class F> std::shared_ptr<std::remove_reference_t<F>> holdshared(F&& f) { return std::make_shared<std::remove_reference_t<F>>(f); }
template<class F> std::shared_ptr<const std::remove_reference_t<F>> holdsharedconst(F&& f) { return std::make_shared<const std::remove_reference_t<F>>(f); }
template<class T> struct PtrTag {};
template<class F> class Tagged { PtrTag<F*>* p; F f; public: Tagged(F g) : p(nullptr), f(g) {} int run(int x) { return p ? 0 : f(x); } };
template<class F> Tagged<std::remove_reference_t<F>> holdtagged(F&& f) { return Tagged<std::remove_reference_t<F>>(f); }
template<class T> int runat0(T& t) { return t[0](5); }
// A user destructor is no ownership evidence: these borrow &f (direct, through a base, nested).
template<class T> struct DtorBorrow { T* p; ~DtorBorrow() {} int run(int x) { return (*p)(x); } };
template<class F> DtorBorrow<std::remove_reference_t<F>> holddtorborrow(F&& f) { return { &f }; }
template<class T> struct DtorPtrBase { T* p; };
template<class T> struct DtorDerived : DtorPtrBase<T> { DtorDerived(T& t) { this->p = &t; } ~DtorDerived() {} int run(int x) { return (*this->p)(x); } };
template<class F> DtorDerived<std::remove_reference_t<F>> holddtorderived(F&& f) { return { f }; }
template<class T> struct DtorInner { T* p; int run(int x) { return (*p)(x); } };
template<class T> struct DtorOuter { DtorInner<T> inner; ~DtorOuter() {} int run(int x) { return inner.run(x); } };
template<class F> DtorOuter<std::remove_reference_t<F>> holddtorouter(F&& f) { return { { &f } }; }
// MSVC shared_ptr shape: a trivial-dtor base holds T*, the derived owner (non-trivial dtor, T by value) frees it.
template<class T> class OwnPtrBase { protected: T* p = nullptr; OwnPtrBase() = default; ~OwnPtrBase() = default; };
template<class T> class OwnedBox : public OwnPtrBase<T> { public: OwnedBox(const T& t) { this->p = new T(t); } OwnedBox(const OwnedBox& o) { this->p = new T(*o.p); } ~OwnedBox() { delete this->p; } int run(int x) { return (*this->p)(x); } };
template<class F> OwnedBox<std::remove_reference_t<F>> holdownedbox(F&& f) { return OwnedBox<std::remove_reference_t<F>>(f); }
template<class T> class BorrowBox : public OwnPtrBase<T> { public: BorrowBox(T& t) { this->p = &t; } int run(int x) { return (*this->p)(x); } };
template<class A, class B> class BoxPair { A a; B b; public: BoxPair(const A& x, const B& y) : a(x), b(y) {} int run(int x) { return b.run(x); } };
template<class F> BoxPair<OwnedBox<std::remove_reference_t<F>>, BorrowBox<std::remove_reference_t<F>>> holdboxpair(F&& f) { return { OwnedBox<std::remove_reference_t<F>>(f), BorrowBox<std::remove_reference_t<F>>(f) }; }
template<class T> int run0(T& t) { return std::get<0>(t)(5); }
template<class T> int runopt(T& t) { return (*t)(5); }
template<class F> struct Tag { int run(int x) const { return x; } };
template<class F> Tag<F> tagonly(F&&) { return {}; }
struct ClosureFactory {
    template<class F> ClosureHolder<F> holdfwd(F&& f) { return ClosureHolder<F>{ static_cast<F&&>(f) }; }
};
inline ClosureFactory closureFactory;
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
