#pragma once
// T22: C++20 view shapes - constrained member overloads, an empty [[no_unique_address]] pair,
// static begin/end, and an empty-class variable template (iota/drop/elements/join/empty views).
#include <concepts>
namespace cflat_t22 {
// iota_view<int, int>::end(): the more constrained end() returns the iterator.
template<class S, class B>
struct Bounded {
private:
    class Sentinel {
        B bound_;
    public:
        explicit Sentinel(B bound) : bound_(bound) {}
        friend bool operator==(const S* it, const Sentinel& s) { return *it == s.bound_; }
    };
public:
    const S* first;
    B count;
    const S* begin() const { return first; }
    const S* end() const requires std::same_as<S, B> { return first + count; }
    Sentinel end() const { return Sentinel(count); }
};
struct IntSpan {
    const int* first;
    const int* last;
    const int* begin() const { return first; }
    const int* end() const { return last; }
};
// reverse_view: an unconstrained begin() and a constrained one with the same signature.
template<class R>
struct Pick {
    R base;
    int which() { return 1; }
    int which() requires std::same_as<R, IntSpan> { return 2; }
};
// elements_view: the non-const begin() is excluded by its constraint, never instantiated.
template<class R, bool Simple>
struct Elements {
    R base;
    auto begin() requires (!Simple) { return base.begin() + 100; }
    auto begin() const requires Simple { return base.begin(); }
    auto end() const requires Simple { return base.end(); }
};
// drop_view: a constrained defaulted default constructor over a member whose default
// initializer is ill-formed for this R must not poison the members that read it.
template<class R>
struct Drop {
    R base = R();
    int count = 0;
    Drop() requires std::default_initializable<R> = default;
    Drop(R r, int n) : base(r), count(n) {}
    const int* begin() const requires std::copyable<R> { return base.begin() + count; }
    const int* end() const { return base.end(); }
};
struct NoDefault {
    const int* first;
    const int* last;
    NoDefault(const int* f, const int* l) : first(f), last(l) {}
    const int* begin() const { return first; }
    const int* end() const { return last; }
};
// join_view: two empty [[no_unique_address]] caches of one type cannot share an address,
// so clang's record is 24 bytes with the second cache past both pointers.
struct EmptyCache {};
struct Joined {
    const int* first;
    const int* last;
    [[no_unique_address]] EmptyCache outer;
    [[no_unique_address]] EmptyCache inner;
    const int* begin() const { return first; }
    const int* end() const { return last; }
};
inline Joined make_joined(const int* data, int count) { return Joined{data, data + count, {}, {}}; }
// empty_view / views::empty<T>: static begin/end, bound through a variable template.
inline constexpr int digits[3] = {1, 2, 3};
template<class T>
struct StaticDigits {
    static const int* begin() { return digits; }
    static const int* end() { return digits + 3; }
};
template<class T>
inline constexpr StaticDigits<T> static_digits{};
// Twins with a default argument are not pruned: pick() can only call the unconstrained one.
template<class T>
struct DefaultOnLoser {
    int pick(int x = 1) { return x + 10; }
    int pick(int x) requires std::integral<T> { return x + 20; }
};
// Two parameters: the twin's own trailing default still leaves pick() to the unconstrained one.
template<class T>
struct DefaultPrefix {
    int pick(int a = 1, int b = 2) { return a + b + 10; }
    int pick(int a, int b = 3) requires std::integral<T> { return a + b + 20; }
};
// An instance f of another class must not hide cppi_entity::Mixed's static f(int) through
// an object (the static-or-instance selection took the first method's `this` of any class).
struct InstanceF {
    int f(int x) { return x + 1; }
};
}
