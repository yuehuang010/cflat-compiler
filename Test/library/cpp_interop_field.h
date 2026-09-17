// M96: a C++ record whose FIELD type is itself a class - a nested member class, or a
// class-template specialization. Both were embedded as opaque bytes because the field's record
// was not laid out (nested) or not registered at all (specialization) before its owner.
#pragma once

namespace cppf {

// Field of a NAMED top-level class - the shape that already worked; kept as the accept-set anchor.
struct Plain { int p; Plain() : p(3) {} int dbl() const { return p * 2; } };
struct Named { Plain pl; Named() {} int get() const { return pl.p; } };

// Nested member class held by value. Clang's traversal emits Inner AFTER Outer.
struct Outer {
    struct Inner { int x; Inner() : x(0) {} int twice() const { return x * 2; } };
    Inner in;
    int tail;
    Outer() : tail(1) { in.x = 21; }
};

// Two levels of nesting, so the ordering has to be transitive.
struct Deep {
    struct Mid {
        struct Leaf { int l; Leaf() : l(5) {} int inc() const { return l + 1; } };
        Leaf lf;
        int m;
        Mid() : m(9) {}
    };
    Mid md;
    Deep() { md.lf.l = 7; }
};

// Fixed array of a nested class type.
struct ArrHost {
    struct Cell { int c; Cell() : c(0) {} int plus(int a) const { return c + a; } };
    Cell cells[2];
    ArrHost() { cells[0].c = 11; cells[1].c = 22; }
};

// POINTER to a nested class: a handle needs no layout, so it must NOT force the field's record
// to be laid out, and the other fields must stay reachable.
struct PtrHost {
    struct Node { int n; Node() : n(0) {} };
    Node* p;
    int local;
    PtrHost() : p(0), local(4) {}
};

// A nested class pointing back at its owner - the ordering walk must not cycle.
struct SelfRef {
    struct Back { SelfRef* owner; int b; Back() : owner(0), b(8) {} };
    Back bk;
    int v;
    SelfRef() : v(2) { bk.owner = this; }
};

template <class T> struct RefTpl { T v; RefTpl() : v(T()) {} T get() const { return v; } };

// Field of a class-template SPECIALIZATION, spelled directly and through an alias. Both name the
// same canonical specialization, so both must resolve to the one registration.
struct HoldsSpec { RefTpl<int> h; int tail; HoldsSpec() : tail(1) { h.v = 6; } };
using AliasSpec = RefTpl<int>;
struct HoldsAlias { AliasSpec a; int tail; HoldsAlias() : tail(2) { a.v = 13; } };

// Specialization instantiated with the OWNER's own nested class.
struct SpecOfNested {
    struct N { int k; N() : k(0) {} };
    RefTpl<N> s;
    SpecOfNested() { s.v.k = 17; }
};

// Specialization whose template has a non-trivial ctor/dtor: the owner's destruction must run it.
inline int& dtor_slot() { static int n = 0; return n; }
inline int dtor_count() { return dtor_slot(); }
template <class T> struct Owner {
    T* buf;
    Owner() : buf(new T(30)) {}
    ~Owner() { ++dtor_slot(); delete buf; }
    T value() const { return *buf; }
};
struct HoldsOwner { Owner<int> o; int tail; HoldsOwner() : tail(3) {} };

}
