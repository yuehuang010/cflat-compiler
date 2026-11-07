#pragma once

namespace cppternary {
struct Item {
    inline static int ctors = 0;
    inline static int copies = 0;
    inline static int moves = 0;
    inline static int dtors = 0;
    inline static int live = 0;
    inline static int which = 0;
    inline static int plusCalls = 0;
    inline static int memberCalls = 0;
    inline static void reset() noexcept {
        ctors = copies = moves = dtors = which = plusCalls = memberCalls = 0;
    }
    explicit Item(int x) noexcept : value(x) { ++ctors; ++live; }
    Item(const Item& other) noexcept : value(other.value) { ++copies; ++live; }
    Item(Item&& other) noexcept : value(other.value) {
        other.value = -1;
        ++moves;
        ++live;
    }
    ~Item() noexcept { --live; ++dtors; }
    Item operator*(Item rhs) const noexcept {
        ++memberCalls;
        which = 6;
        return Item(value * rhs.value);
    }
    int value;
};

inline Item make(int x) noexcept { return Item(x); }
inline int use(Item value) noexcept { Item::which = 1; return value.value; }
inline int use2(Item first, Item second) noexcept {
    Item::which = 2;
    return first.value * 10 + second.value;
}
inline Item operator+(Item first, Item second) noexcept {
    ++Item::plusCalls;
    Item::which = 5;
    return Item(first.value + second.value);
}
inline Item retPlus(bool pick) noexcept {
    return (pick ? Item(1) : Item(2)) + Item(3);
}
inline bool operator==(Item lhs, Item rhs) noexcept { Item::which = 7; return lhs.value == rhs.value; }
inline int operator<<(Item lhs, int shift) noexcept { Item::which = 8; return lhs.value << shift; }
inline int operator-(Item operand) noexcept { Item::which = 9; return -operand.value; }
inline int operator~(Item operand) noexcept { Item::which = 10; return ~operand.value; }
struct Holder {
    inline int use(Item value) const noexcept { Item::which = 3; return value.value; }
};
struct Box {
    inline explicit Box(Item value) noexcept : value(value.value) { Item::which = 4; }
    int value;
};
// SHIFTCH: chained by-value shifts on a prvalue; member and free operator, same trace as clang.
struct Shift {
    inline static int ctors = 0;
    inline static int copies = 0;
    inline static int moves = 0;
    inline static int dtors = 0;
    inline static int live = 0;
    inline static int lastDtor = 0;
    inline static void reset() noexcept { ctors = copies = moves = dtors = lastDtor = 0; }
    explicit Shift(int x) noexcept : value(x) { ++ctors; ++live; }
    Shift(const Shift& other) noexcept : value(other.value) { ++copies; ++live; }
    Shift(Shift&& other) noexcept : value(other.value) { other.value = -other.value; ++moves; ++live; }
    ~Shift() noexcept { --live; ++dtors; lastDtor = value; }
    Shift operator<<(int s) const noexcept { return Shift(value << s); }
    int value;
};
inline Shift operator>>(Shift c, int s) noexcept { return Shift(c.value >> s); }
}


namespace cppternbase {
inline int copies = 0;
inline int dtors = 0;
inline int assignments = 0;
inline int which = 0;

struct B {
    int v;
    B(int value) : v(value) {}
    B(const B& other) : v(other.v) { ++copies; }
    B& operator=(const B& other) { v = other.v; ++assignments; which = 4; return *this; }
    ~B() { ++dtors; }
};

struct D : public B {
    D(int value) : B(value) {}
};

struct D2 : public D {
    D2(int value) : D(value + 100) {}
};

struct X : public B { X(int value) : B(value) {} };
struct Y : public B { Y(int value) : B(value + 1) {} };
struct Amb : public X, public Y { Amb(int value) : X(value), Y(value + 2) {} };
struct Priv : private B { Priv(int value) : B(value) {} };
struct Other { int v; Other(int value) : v(value) {} };

inline void resetCounts() { copies = dtors = assignments = which = 0; }
inline int constRef(const B& value) { which = 1; return value.v; }
inline int mutRef(B& value) { which = 2; value.v += 1000; return value.v; }
inline int byValue(B value) { which = 3; return value.v; }
inline int memberPointer(const B& value)
{
    int B::*member = &B::v;
    which = 6;
    return value.*member;
}
inline int takeD(const D& value) { which = 7; return value.v + 200; }
inline int takeB(const B& value) { which = 8; return value.v + 300; }
}
