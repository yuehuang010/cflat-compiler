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
inline int opCtors = 0, opCopies = 0, opMoves = 0, opDtors = 0, opLive = 0, opWhich = 0;
inline void resetOpCounts() noexcept { opCtors = opCopies = opMoves = opDtors = opWhich = 0; }
struct FriendShift {
    explicit FriendShift(int x) noexcept : value(x) { ++opCtors; ++opLive; }
    FriendShift(const FriendShift& other) noexcept : value(other.value) { ++opCopies; ++opLive; }
    FriendShift(FriendShift&& other) noexcept : value(other.value) { other.value = -1; ++opMoves; ++opLive; }
    ~FriendShift() noexcept { --opLive; ++opDtors; }
    int value;
    friend int operator<<(FriendShift lhs, int shift) noexcept { opWhich = 1; return lhs.value << shift; }
};
struct TemplateShift {
    explicit TemplateShift(int x) noexcept : value(x) { ++opCtors; ++opLive; }
    TemplateShift(const TemplateShift& other) noexcept : value(other.value) { ++opCopies; ++opLive; }
    TemplateShift(TemplateShift&& other) noexcept : value(other.value) { other.value = -1; ++opMoves; ++opLive; }
    ~TemplateShift() noexcept { --opLive; ++opDtors; }
    int value;
};
template <class T> inline int operator<<(TemplateShift lhs, T shift) noexcept {
    opWhich = 2;
    return lhs.value << shift;
}
struct RefShift {
    explicit RefShift(int x) noexcept : value(x) { ++opCtors; ++opLive; }
    RefShift(const RefShift& other) noexcept : value(other.value) { ++opCopies; ++opLive; }
    RefShift(RefShift&& other) noexcept : value(other.value) { other.value = -1; ++opMoves; ++opLive; }
    ~RefShift() noexcept { --opLive; ++opDtors; }
    int value;
};
inline int operator<<(const RefShift& lhs, int shift) noexcept {
    opWhich = 3;
    return lhs.value << shift;
}
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

namespace e6mv {
inline int moves = 0;

struct Tc {
    int v;
    Tc() : v(0) {}
    Tc(int x) : v(x) {}
    Tc(const Tc&) = default;
    template <class U> Tc(U&& u) : v(u.v + 100) { ++moves; }
};

struct NoMove {
    int v;
    NoMove() : v(0) {}
    NoMove(int x) : v(x) {}
    NoMove(const NoMove&) = default;
    NoMove(NoMove&&) = delete;
};

struct Plain {
    int v;
    Plain() : v(0) {}
    Plain(int x) : v(x) {}
    Plain(const Plain&) = default;
};

struct UserMove {
    int v;
    UserMove() : v(0) {}
    UserMove(int x) : v(x) {}
    UserMove(const UserMove&) = default;
    UserMove(UserMove&& other) noexcept : v(other.v + 100) {
        other.v = -1;
        ++moves;
    }
    UserMove& operator=(const UserMove&) = default;
    UserMove& operator=(UserMove&& other) noexcept {
        v = other.v + 100;
        other.v = -1;
        ++moves;
        return *this;
    }
};

struct TcHolder { Tc t; };
struct NoMoveHolder { NoMove t; };
struct PlainHolder { Plain t; };
struct UserMoveHolder { UserMove t; };

// Deleted move ctor + assignment template: an xvalue join assigns through the template (clang 207/1).
struct AssignT {
    int v;
    AssignT(int x) : v(x) {}
    AssignT(const AssignT&) = default;
    AssignT(AssignT&&) = delete;
    template <class U> AssignT& operator=(U&& u) { v = u.v + 200; ++moves; return *this; }
};
struct AssignTHolder { AssignT t; AssignTHolder() : t(0) {} };

inline int take(Tc value) { return value.v; }
inline int take(NoMove value) { return value.v; }
inline int take(Plain value) { return value.v; }
inline int take(UserMove value) { return value.v; }
inline void reset() { moves = 0; }

// TERNFLD r2: counting class whose copy- and move-assignments are visible (clang: copy-assign
// binds the const join, move-assign the mutable one).
struct MvCnt {
    inline static int ctors = 0, copies = 0, moves = 0, copyAssigns = 0, moveAssigns = 0, dtors = 0;
    inline static void reset() noexcept { ctors = copies = moves = copyAssigns = moveAssigns = dtors = 0; }
    int v;
    MvCnt() noexcept : v(0) { ++ctors; }
    explicit MvCnt(int x) noexcept : v(x) { ++ctors; }
    MvCnt(const MvCnt& o) noexcept : v(o.v) { ++copies; }
    MvCnt(MvCnt&& o) noexcept : v(o.v) { o.v = -1; ++moves; }
    MvCnt& operator=(const MvCnt& o) noexcept { v = o.v; ++copyAssigns; return *this; }
    MvCnt& operator=(MvCnt&& o) noexcept { v = o.v; o.v = -1; ++moveAssigns; return *this; }
    ~MvCnt() noexcept { ++dtors; }
};
// TERNFLD r3: field-leaf shapes for the operand-typed const check (clang: cv of the last operand).
struct MvNode { MvCnt f = MvCnt(21); };
struct MvRPtr { MvNode* q; explicit MvRPtr(MvNode* n) noexcept : q(n) {} };
struct MvQPtr { const MvNode* q; explicit MvQPtr(const MvNode* n) noexcept : q(n) {} };
struct MvCF { const MvCnt f{21}; };
struct MvAF { const MvCnt f[2] = {MvCnt(21), MvCnt(23)}; };
// Converting source for a field join: `MvCnt z = k ? move s.x : move b;` (one conversion).
struct MvX {
    inline static int conversions = 0;
    inline static void reset() noexcept { conversions = 0; }
    int v;
    explicit MvX(int x) noexcept : v(x) {}
    operator MvCnt() const { ++conversions; return MvCnt(v + 100); }
};
}
