#pragma once
// Template deduction from CFlat arguments whose C++ type is not their lowered width: built-in
// arithmetic on long / unsigned long (both i64 here, as is long long), enumerators (prvalues)
// and unscoped enums in arithmetic (promoted). Each template reports which T it deduced.
namespace tplid {
template <class A, class B> struct same { static constexpr bool value = false; };
template <class A> struct same<A, A> { static constexpr bool value = true; };
template <class T> struct bare { using type = T; };
template <class T> struct bare<T&> { using type = T; };
template <class T> struct bare<const T> { using type = T; };
template <class T> struct bare<const T&> { using type = T; };
enum E : unsigned { e = 4 };
enum F { f = 5 };
template <class T> constexpr long code() {
    using U = typename bare<T>::type;
    return same<U, E>::value ? 1 : same<U, unsigned>::value ? 3 : same<U, long>::value ? 4
        : same<U, long long>::value ? 5 : same<U, unsigned long>::value ? 6
        : same<U, int>::value ? 7 : same<U, unsigned long long>::value ? 8 : 99;
}
// Lvalue overload answers code*10+1, rvalue overload code*10+2.
struct W {
    long id;
    template <class T> W(T&) : id(code<T>() * 10 + 1) {}
    template <class T> W(T&&) : id(code<T>() * 10 + 2) {}
};
struct M {
    template <class T> long val(T&) { return code<T>() * 10 + 1; }
    template <class T> long val(T&&) { return code<T>() * 10 + 2; }
};
template <class T> long byval(T) { return code<T>() * 10 + 3; }
template <class T> long fwd(T&&) { return code<T>() * 10 + 4; }
// A non-template sibling: `ul + 1` must rank the exact unsigned long specialization first.
inline long mix(long) { return 1; }
template <class T> long mix(T) { return code<T>() * 10 + 3; }
// Brace overloads whose element types share one machine width (long vs long long here, int vs
// long on LLP64): each call ranks its own elements, and the selectors never collide.
struct Pick {
    const void* p; int which;
    Pick(const long (&a)[2]) : p(a), which(1) {}
    Pick(const long long (&a)[2]) : p(a), which(2) {}
};
__attribute__((noinline)) inline long pick(Pick a)
{
    return a.which == 1 ? ((const long*)a.p)[1] * 10 + 1 : ((const long long*)a.p)[1] * 10 + 2;
}
// Operator templates and ternary arms: the deduced T of `1e3 + 1` is double, of `l + 1` long.
template <class T> constexpr long fcode() {
    using U = typename bare<T>::type;
    return same<U, float>::value ? 11 : same<U, double>::value ? 12 : code<T>();
}
struct Op { long last; };
template <class T> long operator*(Op, T) { return fcode<T>(); }
template <class T> long operator<(Op, T) { return fcode<T>(); }
template <class T> long operator<<(Op, T) { return fcode<T>(); }
template <class T> Op& operator*=(Op& o, T) { o.last = fcode<T>(); return o; }
template <class T> long operator-(T, Op) { return fcode<T>(); }
template <class T> long ffwd(T&&) { return fcode<T>(); }
}
