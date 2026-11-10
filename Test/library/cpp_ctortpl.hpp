#pragma once
#include <type_traits>
#include <cstddef>

namespace ctortpl {
inline int calls = 0;
inline int picked = 0;

enum Named { NamedValue = 3 };
enum { AnonValue = 3 };

struct Both {
    int value = 0;
    Both(int x) : value(x) { picked = 1; }
    template<class U, std::enable_if_t<std::is_enum_v<U>, int> = 0>
    Both(U x) : value(static_cast<int>(x) + 100) { picked = 2; ++calls; }
};

enum { FlagA = 1, FlagB = 2 };

// Multi-argument: the enum template's first parameter outranks the int pair for an unnamed enum.
struct Two {
    int value = 0;
    Two(int x, int y) : value(x + y) { picked = 1; }
    template<class U, std::enable_if_t<std::is_enum_v<U>, int> = 0>
    Two(U x, int y) : value(static_cast<int>(x) + y + 100) { picked = 2; }
};

struct NullPF {
    int value = 0;
    NullPF(int x, int*) : value(x) { picked = 1; }
    template<class U, std::enable_if_t<std::is_enum_v<U>, int> = 0>
    NullPF(U x, int*) : value(static_cast<int>(x) + 100) { picked = 2; ++calls; }
};
struct NullPFReceiver { int accept(NullPF p) { return p.value; } };

struct DeletedEnum {
    int value = 0;
    DeletedEnum(int x) : value(x) { picked = 1; }
    template<class U, std::enable_if_t<std::is_enum_v<U>, int> = 0>
    DeletedEnum(U) = delete;
};
struct DeletedEnumReceiver { int accept(DeletedEnum p) { return p.value; } };

enum AmbiguousEnum { AmbiguousValue = 3 };
struct AmbiguousD5 {
    int value = 0;
    AmbiguousD5(decltype(AmbiguousValue) x, int = 0) : value(100 + static_cast<int>(x)) {}
    AmbiguousD5(decltype(AmbiguousValue) x) : value(200 + static_cast<int>(x)) {}
};
struct AmbiguousD5Receiver { int accept(AmbiguousD5 p) { return p.value; } };

struct D5Single {
    int value = 0;
    D5Single(decltype(AmbiguousValue) x, int = 0) : value(300 + static_cast<int>(x)) {}
};

struct Forward {
    int value = 0;
    Forward() = default;
    explicit Forward(int x) : value(x) {}
    Forward(const Forward&) = default;
    Forward(Forward&&) = default;
    Forward& operator=(const Forward&) = default;
    template<class U>
    Forward(U&& x) : value(x.value + 100) { x.value = -1; ++calls; }
};
static_assert(std::is_trivially_copyable_v<Forward>);

struct NoTemplate { int value = 0; };

struct Away {
    int value = 0;
    Away() = default;
    Away(const Away&) = default;
    template<class U> requires std::is_same_v<U, double>
    Away(U&& x) : value(static_cast<int>(x) + 100) { ++calls; }
};
static_assert(std::is_trivially_copyable_v<Away>);

inline Forward makeForward(int x) { return Forward(x); }
inline int takeForward(Forward x) { return x.value; }
inline Forward returnForward() { Forward x(90); return x; }

// ctorconv: derived lvalue slicing, evaluated scalar defaults, and moved subscript arguments.
inline int sliceBaseInt = 0, sliceBaseCopy = 0, sliceBaseDtor = 0;
inline int sliceDerivedCtor = 0, sliceDerivedDtor = 0;
struct SliceBase {
    int value;
    SliceBase(int x) : value(x) { ++sliceBaseInt; }
    SliceBase(const SliceBase& other) : value(other.value) { ++sliceBaseCopy; }
    ~SliceBase() { ++sliceBaseDtor; }
};
struct SliceDerived : SliceBase {
    SliceDerived(int x) : SliceBase(x) { ++sliceDerivedCtor; }
    ~SliceDerived() { ++sliceDerivedDtor; }
};
inline void resetSlice() {
    sliceBaseInt = sliceBaseCopy = sliceBaseDtor = 0;
    sliceDerivedCtor = sliceDerivedDtor = 0;
}

inline int trivialSliceBaseCtor = 0, trivialSliceDerivedCtor = 0;
inline int trivialSliceNextCount = 0;
struct TrivialSliceBase {
    int value;
    TrivialSliceBase(int x) : value(x) { ++trivialSliceBaseCtor; }
};
struct TrivialSliceDerived : TrivialSliceBase {
    TrivialSliceDerived(int x) : TrivialSliceBase(x) { ++trivialSliceDerivedCtor; }
};
struct TrivialSliceX { int value; };
struct TrivialSliceMultiple : TrivialSliceX, TrivialSliceBase {
    TrivialSliceMultiple(int x) : TrivialSliceX{x + 100}, TrivialSliceBase(x) {
        ++trivialSliceDerivedCtor;
    }
};
static_assert(std::is_trivially_copyable_v<TrivialSliceBase>);
static_assert(std::is_trivially_copyable_v<TrivialSliceDerived>);
static_assert(std::is_trivially_copyable_v<TrivialSliceMultiple>);
inline void resetTrivialSlice() {
    trivialSliceBaseCtor = trivialSliceDerivedCtor = trivialSliceNextCount = 0;
}
inline int nextTrivialSliceIndex() { return trivialSliceNextCount++; }

inline int dynamicNextCount = 0, dynamicGlobal = 40;
inline int dynamicNextCtor = 0, dynamicGlobalCtor = 0, dynamicDtor = 0;
inline int nextDynamicDefault() { return ++dynamicNextCount; }
struct DynamicNext {
    int value;
    DynamicNext(Named e, int w = nextDynamicDefault())
        : value(static_cast<int>(e) * 100 + w) { ++dynamicNextCtor; }
    ~DynamicNext() { ++dynamicDtor; }
};
struct DynamicArithmetic {
    int value;
    DynamicArithmetic(int x, int w = nextDynamicDefault())
        : value(x * 100 + w) { ++dynamicNextCtor; }
    ~DynamicArithmetic() { ++dynamicDtor; }
};
struct DynamicGlobal {
    int value;
    DynamicGlobal(Named e, int w = dynamicGlobal)
        : value(static_cast<int>(e) * 100 + w) { ++dynamicGlobalCtor; }
    ~DynamicGlobal() { ++dynamicDtor; }
};
inline void resetDynamic() {
    dynamicNextCount = dynamicNextCtor = dynamicGlobalCtor = dynamicDtor = 0;
    dynamicGlobal = 40;
}
inline int classDefaultArgCtor = 0, classDefaultArgDtor = 0;
inline int classDefaultCtor = 0, classDefaultDtor = 0;
struct ClassDefaultArg {
    int value;
    ClassDefaultArg(int x) : value(x) { ++classDefaultArgCtor; }
    ~ClassDefaultArg() { ++classDefaultArgDtor; }
};
struct DynamicClassDefault {
    int value;
    DynamicClassDefault(Named e, ClassDefaultArg arg = ClassDefaultArg(7))
        : value(static_cast<int>(e) * 100 + arg.value) { ++classDefaultCtor; }
    ~DynamicClassDefault() { ++classDefaultDtor; }
};
inline void resetClassDefault() {
    classDefaultArgCtor = classDefaultArgDtor = classDefaultCtor = classDefaultDtor = 0;
}

inline int moveIndexCtor = 0, moveIndexDtor = 0;
struct MoveIndex {
    int value;
    MoveIndex(int x) : value(x) { ++moveIndexCtor; }
    ~MoveIndex() { ++moveIndexDtor; }
};
inline void resetMoveIndex() { moveIndexCtor = moveIndexDtor = 0; }
}
