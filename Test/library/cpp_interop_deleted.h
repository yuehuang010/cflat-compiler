#pragma once

namespace cppdel {
inline int free_deleted(int) = delete;
inline int free_pick(int) { return 116; }
int free_pick(long) = delete;
inline int free_best(long) { return 1; }
int free_best(int) = delete;

struct Member {
    int call(int) const = delete;
    static int stat(int) = delete;
};

struct MemberRank {
    int call(int) const = delete;
    int call(double) const { return 8; }
    static int stat(int) = delete;
    static int stat(double) { return 9; }
};

template<class T> inline int templ(T) { return 1; }
template<> int templ<int>(int) = delete;

struct FreeOp { int value; };
inline int operator+(const FreeOp&, int) = delete;

struct MemberOp {
    MemberOp operator+(int) const = delete;
};

struct MemberOpRank {
    MemberOpRank operator+(int) const = delete;
    int operator+(double) const { return 7; }
};

struct Conversion {
    operator int() const = delete;
};

struct Assignment {
    ~Assignment() {}
    Assignment& operator=(const Assignment&) = delete;
};

struct MoveAssignDeleted {
    int v;
    MoveAssignDeleted(int x) : v(x) {}
    MoveAssignDeleted(const MoveAssignDeleted&) = default;
    MoveAssignDeleted(MoveAssignDeleted&&) noexcept = default;
    MoveAssignDeleted& operator=(const MoveAssignDeleted& other) { v = other.v; return *this; }
    MoveAssignDeleted& operator=(MoveAssignDeleted&&) = delete;
    ~MoveAssignDeleted() {}
};

struct RvalueArg { int x; };
struct RvalueSibling {
    int f(RvalueArg&&) = delete;
    int f(RvalueArg&) { return 5; }
    int h(RvalueArg&&) & = delete;
    int h(RvalueArg&) & { return 5; }
};

struct VolRecvA { int f() { return 101; } int f() const { return 102; } };
struct VolRecvADeleted { int f() { return 101; } int f() const = delete; };
struct VolRecvB { int f() { return 201; } int f() volatile { return 202; } };
struct VolRecvBDeleted { int f() { return 201; } int f() volatile = delete; };
struct VolRecvC { int f() const { return 301; } int f() volatile { return 302; } };
struct VolRecvCDeleted { int f() const { return 301; } int f() volatile = delete; };
struct VolRecvD { int f() const volatile { return 401; } };
struct VolRecvDDeleted { int f() const volatile = delete; };
struct VolRecvE { int f() & { return 501; } int f() const & { return 502; } };
struct VolRecvEDeleted { int f() & { return 501; } int f() const & = delete; };

#define CPPDEL_T122_RECEIVERS(Type) \
    inline Type& t122_plain_ref_##Type(Type& x) { return x; } \
    inline const Type& t122_const_ref_##Type(Type& x) { return x; } \
    inline volatile Type& t122_volatile_ref_##Type(Type& x) { return x; } \
    inline const volatile Type& t122_cv_ref_##Type(Type& x) { return x; } \
    inline Type* t122_plain_ptr_##Type(Type* x) { return x; } \
    inline const Type* t122_const_ptr_##Type(Type* x) { return x; } \
    inline volatile Type* t122_volatile_ptr_##Type(Type* x) { return x; } \
    inline const volatile Type* t122_cv_ptr_##Type(Type* x) { return x; } \
    inline volatile Type& t122_volatile_object_##Type(Type& x) { return x; } \
    inline const volatile Type& t122_cv_object_##Type(Type& x) { return x; }

CPPDEL_T122_RECEIVERS(VolRecvA)
CPPDEL_T122_RECEIVERS(VolRecvADeleted)
CPPDEL_T122_RECEIVERS(VolRecvB)
CPPDEL_T122_RECEIVERS(VolRecvBDeleted)
CPPDEL_T122_RECEIVERS(VolRecvC)
CPPDEL_T122_RECEIVERS(VolRecvCDeleted)
CPPDEL_T122_RECEIVERS(VolRecvD)
CPPDEL_T122_RECEIVERS(VolRecvDDeleted)
CPPDEL_T122_RECEIVERS(VolRecvE)
CPPDEL_T122_RECEIVERS(VolRecvEDeleted)
#undef CPPDEL_T122_RECEIVERS
}

namespace rv { namespace a {
inline int del(int) = delete;
} }

namespace cppdelrv {
// A const prvalue cannot bind T&&: clang copy-assigns it.
struct ConstPrvalueAssign {
    int v;
    ConstPrvalueAssign(int x) : v(x) {}
    ConstPrvalueAssign(const ConstPrvalueAssign&) = default;
    ConstPrvalueAssign(ConstPrvalueAssign&&) = default;
    ConstPrvalueAssign& operator=(const ConstPrvalueAssign& other) { v = 100 + other.v; return *this; }
    ConstPrvalueAssign& operator=(ConstPrvalueAssign&&) = delete;
    ~ConstPrvalueAssign() {}
};
inline const ConstPrvalueAssign makeConstPrvalueAssign(int x) { return ConstPrvalueAssign(x); }

// The deleted move-assign is `&&`-qualified: it is not viable for an lvalue destination, so clang copies.
struct RvalueQualAssign {
    int v;
    RvalueQualAssign(int x) : v(x) {}
    RvalueQualAssign(const RvalueQualAssign&) = default;
    RvalueQualAssign(RvalueQualAssign&&) = default;
    RvalueQualAssign& operator=(const RvalueQualAssign& other) & { v = 100 + other.v; return *this; }
    RvalueQualAssign& operator=(RvalueQualAssign&&) && = delete;
    ~RvalueQualAssign() {}
};

// A member deletes the defaulted move-assign: overload resolution ignores it, so clang copies.
struct NoMoveBlock {
    NoMoveBlock() = default;
    NoMoveBlock(const NoMoveBlock&) = default;
    NoMoveBlock(NoMoveBlock&&) = default;
    NoMoveBlock& operator=(const NoMoveBlock&) { return *this; }
    NoMoveBlock& operator=(NoMoveBlock&&) = delete;
};
struct DefaultedDeletedAssign {
    int v;
    NoMoveBlock b;
    DefaultedDeletedAssign(int x) : v(x) {}
    DefaultedDeletedAssign(const DefaultedDeletedAssign&) = default;
    DefaultedDeletedAssign(DefaultedDeletedAssign&&) = default;
    DefaultedDeletedAssign& operator=(const DefaultedDeletedAssign& other) { v = 100 + other.v; return *this; }
    DefaultedDeletedAssign& operator=(DefaultedDeletedAssign&&) = default;
    ~DefaultedDeletedAssign() {}
};

// The deleted non-const T&& is the better match for a non-const rvalue, over the usable const T&& one.
struct ConstRvalueAssign {
    int v;
    ConstRvalueAssign(int x) : v(x) {}
    ConstRvalueAssign(const ConstRvalueAssign&) = default;
    ConstRvalueAssign(ConstRvalueAssign&&) = default;
    ConstRvalueAssign& operator=(const ConstRvalueAssign& other) { v = 100 + other.v; return *this; }
    ConstRvalueAssign& operator=(ConstRvalueAssign&&) = delete;
    ConstRvalueAssign& operator=(const ConstRvalueAssign&& other) & { v = 200 + other.v; return *this; }
    ~ConstRvalueAssign() {}
};

// A by-value operator=(T) ties with the deleted operator=(T&&) for a prvalue: clang reports ambiguity.
struct ByValueAmbiguous {
    int v;
    ByValueAmbiguous(int x) : v(x) {}
    ByValueAmbiguous(const ByValueAmbiguous&) = default;
    ByValueAmbiguous(ByValueAmbiguous&&) = default;
    ByValueAmbiguous& operator=(ByValueAmbiguous other) { v = 100 + other.v; return *this; }
    ByValueAmbiguous& operator=(ByValueAmbiguous&&) = delete;
    ~ByValueAmbiguous() {}
};

// Same tie with a top-level const by-value parameter.
struct ConstByValueAmbiguous {
    int v;
    ConstByValueAmbiguous(int x) : v(x) {}
    ConstByValueAmbiguous(const ConstByValueAmbiguous&) = default;
    ConstByValueAmbiguous(ConstByValueAmbiguous&&) = default;
    ConstByValueAmbiguous& operator=(const ConstByValueAmbiguous other) { v = 100 + other.v; return *this; }
    ConstByValueAmbiguous& operator=(ConstByValueAmbiguous&&) = delete;
    ~ConstByValueAmbiguous() {}
};
}
