#pragma once

/*
 * M97 - C++ REFERENCE data members. Itanium lays a reference member out exactly as a pointer,
 * so cflat binds one as `T*`: reading the field yields the pointer, `*obj.r` the referent.
 * A store to the field would reseat the reference, which C++ cannot express, so it is refused.
 */
namespace cppref {

struct Val { int a; int b; int sum() const { return a + b; } };

struct RefHolder {
    int& r;
    explicit RefHolder(int& x) : r(x) {}
    int get() const { return r; }
    void set(int v) { r = v; }
};

inline RefHolder make_ref(int& x) { return RefHolder(x); }
inline int take_ref(RefHolder h) { return h.get(); }

struct ClassRef {
    Val& v;
    explicit ClassRef(Val& x) : v(x) {}
    int sum() const { return v.sum(); }
};

struct ConstRef {
    const int& r;
    explicit ConstRef(const int& x) : r(x) {}
    int get() const { return r; }
};

struct RvalRef {
    int&& r;
    explicit RvalRef(int&& x) : r(static_cast<int&&>(x)) {}
    int get() const { return r; }
};

struct PtrRef {
    int*& p;
    explicit PtrRef(int*& x) : p(x) {}
    int get() const { return *p; }
};

// A reference member in a BASE class: this pair goes down the flattened field walk instead.
struct RefBase {
    int& r;
    explicit RefBase(int& x) : r(x) {}
    int base_get() const { return r; }
};
struct RefDerived : RefBase {
    int extra;
    RefDerived(int& x, int e) : RefBase(x), extra(e) {}
    int total() const { return r + extra; }
};

template <class T>
struct TRef {
    T& t;
    explicit TRef(T& x) : t(x) {}
    T get() const { return t; }
    void set(T v) { t = v; }
};

// Ground truth for the layout legs: the C++ side's own sizeof.
inline int size_ref_holder()  { return (int)sizeof(RefHolder); }
inline int size_ref_derived() { return (int)sizeof(RefDerived); }
inline int size_tref_int()    { return (int)sizeof(TRef<int>); }

}
