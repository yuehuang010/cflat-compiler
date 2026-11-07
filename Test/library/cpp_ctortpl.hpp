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
}
