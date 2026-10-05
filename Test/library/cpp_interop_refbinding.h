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

// Function templates with a non-const `U&` parameter: the same rule as the non-template form.
template <class U> int tbump(U& x) { x = x + 1; return (int)x; }
template <class U> int tbump_ex(U& x) { return ++x.v; }
template <class U> int tout(U v, int& out) { out = (int)v; return 1; }
template <class U> int literal_cref_size(const U& value) { return (int)sizeof(U) + (int)value; }
template <class U> int literal_value_size(U value) { return (int)sizeof(U) + (int)value; }
template <class U> int literal_size(const U&) { return (int)sizeof(U); }
inline int literal_concrete_cref(const int& value) { return value; }
template <class... A> int tpack(A&... a) { return (int)sizeof...(a); }
inline int tval(int v) { return v; }
struct TMember { template <class U> int bump(U& x) { x = x + 1; return (int)x; } };
template <class U> struct LiteralRefMember {
    using value_type = U;
    int cref(const value_type& value) { return (int)sizeof(value_type) + (int)value; }
};
// Own namespace: no non-template operator+ sibling (Ex's) that could take the rvalue instead.
namespace tsink {
struct TSink { int t = 0; };
template <class U> int operator+(TSink& s, U& x) { return s.t += (int)x; }
template <class U> int operator<<(TSink& s, U& x) { return s.t += (int)x; }
}

}
