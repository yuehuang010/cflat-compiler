#pragma once

#include <cstddef>

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
    int& ir() { return r; }
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

// Aggregate-only reference members for designated and positional brace initialization.
struct BraceConstRef { const int& r; };
struct BraceRvalRef { int&& r; };
struct FunctionRef {
    int (&f)(int);
    int v;
    int call(int x) const { return f(x); }
};
inline int ref_member_cpp_fn(int x) { return x + 1000; }
inline int (&ref_member_fn_ref)(int) = ref_member_cpp_fn;
inline int (&ref_member_fn_ref_get())(int) { return ref_member_cpp_fn; }
struct FunctionRefDerived : FunctionRef { int w; };
extern "C" inline int ref_member_c_fn(int x) { return x + 2000; }
struct PointerAgg { int* p; int value; };
struct FunctionPointerAgg { int (*f)(int); int value; };
struct NullptrAgg { std::nullptr_t p; int value; };
struct TextPointerAgg { const char* text; void* raw; int value; int code; };
struct RefTextPointerAgg { int& ref; const char* text; void* raw; int value; };
struct NestedPointerAgg { TextPointerAgg inner; int tail; };
inline int null_pick(void*) { return 11; }
inline int null_pick(std::nullptr_t) { return 22; }
inline int pointer_pick(int*) { return 51; }
inline int pointer_pick(std::nullptr_t) { return 52; }
inline int function_pick(int (*)(int)) { return 31; }
inline int function_pick(std::nullptr_t) { return 32; }
inline int read_text(TextPointerAgg value) {
    return value.text[0] + (value.raw == nullptr ? 10 : 0) + value.value + value.code;
}
inline int read_nested(NestedPointerAgg value) { return read_text(value.inner) + value.tail; }
inline int refmemLifetimeCtors = 0;
inline int refmemLifetimeCopies = 0;
inline int refmemLifetimeMoves = 0;
inline int refmemLifetimeDtors = 0;
struct RefMemLifetimeValue {
    int v;
    explicit RefMemLifetimeValue(int value) : v(value) { ++refmemLifetimeCtors; }
    RefMemLifetimeValue(const RefMemLifetimeValue& other) : v(other.v) { ++refmemLifetimeCopies; }
    RefMemLifetimeValue(RefMemLifetimeValue&& other) : v(other.v) { ++refmemLifetimeMoves; }
    ~RefMemLifetimeValue() { ++refmemLifetimeDtors; }
};
struct RefMemLifetimeConstRef { const RefMemLifetimeValue& c; };
struct RefMemLifetimeRvalueRef { RefMemLifetimeValue&& c; };
inline void reset_refmem_lifetime_counts() {
    refmemLifetimeCtors = refmemLifetimeCopies = refmemLifetimeMoves = refmemLifetimeDtors = 0;
}
inline int refmem_lifetime_ctor_count() { return refmemLifetimeCtors; }
inline int refmem_lifetime_copy_count() { return refmemLifetimeCopies; }
inline int refmem_lifetime_move_count() { return refmemLifetimeMoves; }
inline int refmem_lifetime_dtor_count() { return refmemLifetimeDtors; }
struct BraceRefInner { int& r; int v; double d; };
struct BraceRefOuter { BraceRefInner m; int k; };
struct BraceRefDeep { BraceRefOuter o; };
struct BraceAggNestedRefBase { BraceRefOuter nested; int baseTail; };
struct BraceAggNestedRefDerived : BraceAggNestedRefBase { int derivedTail; };
struct BraceAggNestedPair { int a; int b; };
struct BraceAggObjectInner { int x; BraceAggNestedPair pair; };
struct BraceAggObjectBase { BraceAggObjectInner inner; int& r; };
struct BraceAggObjectDerived : BraceAggObjectBase { int tail; };

inline int refbaseOrderSequence = 0;
inline int refbaseOrderValues[8] = {};
inline void reset_refbase_order() { refbaseOrderSequence = 0; }
inline int refbase_order_value(int value) {
    refbaseOrderValues[refbaseOrderSequence++] = value;
    return value;
}
inline int refbase_order() {
    int result = 0;
    for (int i = 0; i < refbaseOrderSequence; ++i)
        result = result * 10 + refbaseOrderValues[i];
    return result;
}
struct BraceOrderLeaf { int value; };
struct BraceOrderInner { int first; BraceOrderLeaf nested; int last; };
struct BraceOrderOuter { BraceOrderInner inner; int tail; };

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

struct BraceAggRefBase { int& r; };
struct BraceAggRefDerived : BraceAggRefBase { int v; };
struct BraceAggConstRefBase { const int& r; };
struct BraceAggConstRefDerived : BraceAggConstRefBase { int v; };
struct BraceAggRefMid : BraceAggRefBase { int m; };
struct BraceAggRefDeep : BraceAggRefMid { int d; };
struct BraceAggMixedBase { int& r; int b; };
struct BraceAggMixedDerived : BraceAggMixedBase { int d; };
struct BraceAggEmptyBase {};
struct BraceAggEmptyDerived : BraceAggEmptyBase { int& r; int v; };
struct BraceAggD12RefBase { int& r; };
struct BraceAggD12ValueBase { int a; int b; };
struct BraceAggD12 : BraceAggD12RefBase, BraceAggD12ValueBase { int& s; };
struct BraceAggDMRefValueBase { int& r; int a; };
struct BraceAggDMRefBase { int& s; };
struct BraceAggDM : BraceAggDMRefValueBase, BraceAggDMRefBase { int v; };

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
