#pragma once
// GRPLEAK host line: requests on these templates pull the dependency lines into this line's TU.
namespace grpleak_host {
template <class T> struct Box { T item; int size() const { return (int)sizeof(T); } };
template <class A, class B> struct Pair {
    A a; B b;
    int size() const { return (int)(sizeof(A) + sizeof(B)); }
};
struct GrpleakHostMember { int p(int) { return 3; } };
template <class T> struct Calls {
    int sf() { return T::sf(); }
    int g() { return T::g(); }
    int bump() { return T::bump(); }
};
}
