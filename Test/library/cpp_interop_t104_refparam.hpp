#pragma once
namespace t104 {
struct Source { int v = 17; };
struct TemplatedTarget {
    int v;
    template<class T> TemplatedTarget(const T& source) : v(source.v) {}
};
inline int takeTemplated(TemplatedTarget&& value) { return value.v + 600; }

inline int made = 0, gone = 0, copies = 0;
struct X {
    int v;
    X(int value = 71) : v(value) { ++made; }
    X(const X& value) : v(value.v) { ++made; ++copies; }
    ~X() { ++gone; }
};
inline X saved{71};
struct RefSource { operator X&() const { return saved; } };
struct OpBase { operator X() const { return X(91); } };
struct OpDerived : OpBase {};
inline int takeConstX(const X& value) { return &value == &saved ? 1071 : value.v; }
inline int takeOp(X&& value) { return value.v + 200; }

inline int constArray[3] = {4, 5, 6};
struct ConstArrayRef { const int (&r)[3]; };
inline ConstArrayRef constArrayRef() { return {constArray}; }
struct ConstRefSource { operator const X&() const { return saved; } };
struct FakeRefHolder { X& get() const { return saved; } };
struct ExplicitRefSource { explicit operator X&() const { return saved; } };
inline int takeMutX(X& value) { return &value == &saved ? 1 : 0; }
}
