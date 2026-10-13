#pragma once

namespace t138 {
template<class A, class B> struct P {
    A t;
    B u;
    int method() const { return static_cast<int>(u) + 30; }
};

P<int, int> p11{1, 2};
using Alias = P<int, int>;
Alias aliasP{3, 4};
template<class T> using AliasTemplate = P<T, int>;
AliasTemplate<int> aliasT{7, 8};
typedef P<int, int> Named;
Named typedefP{5, 6};
P<P<int, int>, int> nested{{9, 10}, 11};

template<class T> struct Holder {
    static P<int, int> item;
    static int scalar;
};
template<class T> P<int, int> Holder<T>::item{12, 13};
template<class T> int Holder<T>::scalar = 19;

extern P<int, int> external;
inline P<int, int> external{17, 18};
struct Plain { int u; };
Plain plain{16};

inline int is_p11(P<int, int>* value) { return value == &p11 ? 1 : 0; }
inline int route(P<int, int>& value, int) { return 1000 + value.u; }
inline int route(P<int, int> value, double) { return 2000 + value.u; }
}
