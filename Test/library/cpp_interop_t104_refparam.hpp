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

// REFCONV: `operator X&&()` binds its xvalue; reference conversions inherited through a public base.
namespace t104 {
struct RRefSource { operator X&&() const { return static_cast<X&&>(saved); } };
struct DerivedRefSource : RefSource {};
struct DerivedRRefSource : RRefSource {};
struct PrivateRefSource : private RefSource {};
struct ProtectedRefSource : protected RefSource {};
struct PrivateRRefSource : private RRefSource {};
inline int takeRvX(X&& value) { return &value == &saved ? 2071 : value.v; }
inline int pickX(X&& value) { return &value == &saved ? 4071 : 4000; }
inline int pickX(const X& value) { return &value == &saved ? 5071 : 5000; }
inline int takeValX(X value) { return &value == &saved ? -1 : value.v + 3000; }
struct Pad { long long a = 1, b = 2; };
struct OffsetX : Pad, X { OffsetX() : X(73) {} };
inline OffsetX savedOffset;
struct OffsetRefSource { operator OffsetX&() const { return savedOffset; } };
struct OffsetRRefSource { operator OffsetX&&() const { return static_cast<OffsetX&&>(savedOffset); } };
inline int takeOffsetX(const X& value) { return &value == static_cast<const X*>(&savedOffset) ? 1073 : value.v; }
struct BothRefSource {
    operator X&() const { return saved; }
    operator X&&() const { return static_cast<X&&>(saved); }
};
inline int pickCv(X&) { return 1; }
inline int pickCv(const X&) { return 2; }
inline int pickAll(X&) { return 1; }
inline int pickAll(const X&) { return 2; }
inline int pickAll(X&&) { return 3; }
struct DerivedWithRef : X { DerivedWithRef() : X(90) {} operator X&() { return saved; } };
struct RefTaker {
    int take(X&& value) { return &value == &saved ? 7071 : 7000; }
    int takeConst(const X& value) { return &value == &saved ? 7171 : 7100; }
};
}
