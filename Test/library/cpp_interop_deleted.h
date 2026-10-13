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
