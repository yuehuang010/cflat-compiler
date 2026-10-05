#pragma once
// T31: brace-list arguments against std-shaped min/max overload sets. A braced list deduces T
// only against initializer_list<T>; the (const T&, const T&) templates drop out.
#include <initializer_list>

namespace std
{
namespace t31
{
inline int last = 0;  // which overload ran: 2 = pair, 3 = list, 4 = list + compare
inline int which() { return last; }
template <class T> struct mm_pair { T first; T second; };
template <class T> const T& mx(const T& a, const T& b) { last = 2; return a < b ? b : a; }
template <class T> T mx(std::initializer_list<T> l)
{
    last = 3;
    T r = *l.begin();
    for (const T& v : l) if (r < v) r = v;
    return r;
}
template <class T, class C> T mx(std::initializer_list<T> l, C less)
{
    last = 4;
    T r = *l.begin();
    for (const T& v : l) if (less(r, v)) r = v;
    return r;
}
template <class T> mm_pair<T> mnmx(std::initializer_list<T> l)
{
    last = 3;
    mm_pair<T> r{ *l.begin(), *l.begin() };
    for (const T& v : l) { if (v < r.first) r.first = v; if (r.second < v) r.second = v; }
    return r;
}
template <class T> mm_pair<T> mnmx(const T& a, const T& b)
{
    last = 2;
    return a < b ? mm_pair<T>{ a, b } : mm_pair<T>{ b, a };
}
template <class T> std::size_t count(std::initializer_list<T> l) { return l.size(); }
template <class T> const T* first(std::initializer_list<T> l) { return l.begin(); }
struct greater { bool operator()(int a, int b) const { return a > b; } };
// Lifetime of the list's backing array (round 2): a result that may refer into it is refused.
inline int global = 42;
template <class T> const int& stable(std::initializer_list<T>) { return global; }
template <class T> std::initializer_list<T> echo(std::initializer_list<T> l) { return l; }
template <class T> struct view { const T* at; };                // trivially destructible view
template <class T> view<T> view_of(std::initializer_list<T> l) { return view<T>{ l.begin() + 1 }; }
template <class T> struct owner                                  // owns a copy: no borrow
{
    T* data;
    explicit owner(const T* p) : data(new T(*p)) {}
    owner(const owner& o) : data(new T(*o.data)) {}
    ~owner() { delete data; }
};
template <class T> owner<T> owner_of(std::initializer_list<T> l) { return owner<T>(l.begin() + 1); }
template <class T> T pick(std::initializer_list<T> l) { return *(l.begin() + 1); }
// Round 3: the list's elements live in the CALLER frame to the end of the statement (C++'s
// full-expression), so a result referring into them is valid while that statement runs.
inline int cells_dead = 0;
struct cell { int value; cell(int v) : value(v) {} cell(const cell& c) : value(c.value) {} ~cell() { ++cells_dead; } };
struct tally { int sum; int n; ~tally() {} };                     // non-trivial, pointer-free
inline tally sum_of(std::initializer_list<cell> l)
{
    tally t{ 0, 0 };
    for (const cell& c : l) { t.sum += c.value; ++t.n; }
    return t;
}
struct cleanup_view { const cell* at; ~cleanup_view() {} };      // user destructor, still a view
template <class T> cleanup_view cleanup(std::initializer_list<T> l) { return cleanup_view{ l.begin() + 1 }; }
template <class T> const int* member_of(std::initializer_list<T> l) { return &l.begin()->value; }
inline int observe(cleanup_view v) { return cells_dead * 100 + v.at->value; }
inline int observe_value(const int* p) { return cells_dead * 100 + *p; }
inline int dead() { return cells_dead; }
// A list that becomes an array or aggregate temporary cannot be backed by the caller: refused.
template <int N> const int* arr_first(const int (&a)[N]) { return a; }
struct duo { int a; int b; };
template <class T = int> const int* duo_b(const duo& d) { return &d.b; }
// Round 5: a pointer-free result over an array / aggregate temporary of the list.
template <class T> int arr_two(const T (&a)[2]) { return (int)(sizeof(a) / sizeof(a[0])); }
template <class T> struct pairof { T a; T b; };
template <class T> int pair_two(const pairof<T>& p) { return 2; }
// Round 4: a list built in a '?:' arm or a '&&' / '||' operand, a pointer-free result over
// non-trivial elements, and nested lists all die at the end of the full-expression, in reverse.
template <class T> const T* at(std::initializer_list<T> l) { return l.begin(); }
inline int get(const cell* c) { return cells_dead * 100 + c->value; }
template <class T> int number(std::initializer_list<T> l) { return cells_dead * 100 + l.begin()->value; }
inline int order_log = 0;
struct ocell { int v; ocell(int x) : v(x) {} ocell(const ocell& o) : v(o.v) {} ~ocell() { order_log = order_log * 10 + v; } };
template <class T> const T* oat(std::initializer_list<T> l) { return l.begin(); }
inline const ocell& oref(const ocell* o) { return *o; }
inline int oget(const ocell* o) { return o->v; }
inline int olog() { int r = order_log; order_log = 0; return r; }
}
}

namespace t31u
{
inline int last = 0;
inline int which() { return last; }
template <class T> const T& big(const T& a, const T& b) { last = 2; return a < b ? b : a; }
template <class T> T big(std::initializer_list<T> l)
{
    last = 3;
    T r = *l.begin();
    for (const T& v : l) if (r < v) r = v;
    return r;
}
}
