#pragma once
#include <string>
// Section M102: a C++ CONSTRUCTOR parameter declared `T&` must bind the ARGUMENT's address.
// Every class here proves it by writing through the reference after the ctor returned.
namespace cppcr {

struct Cnt { int v; };

// Mutates the referent in the ctor, stores its address, writes again in the dtor.
struct Setter {
    Cnt* p;
    Setter(Cnt& c) : p(&c) { c.v += 1; }
    ~Setter() { p->v += 10; }
};

// const T& - reads only, so a temporary is a legal argument for it.
struct Reader {
    int seen;
    Reader(const Cnt& c) : seen(c.v) {}
    int get() const { return seen; }
};

// A reference DATA MEMBER initialised from a `T&` ctor parameter must alias the argument.
struct RefMem {
    Cnt& r;
    RefMem(Cnt& c) : r(c) {}
    int get() const { return r.v; }
    void bump() { r.v += 5; }
};

// A scalar `int&` ctor parameter with a reference member.
struct HasRef {
    int& r;
    HasRef(int& x) : r(x) {}
    int get() const { return r; }
};

// A referent that cannot be copied at all: a bitwise duplicate models nothing.
struct NoCopy {
    int v;
    NoCopy() : v(0) {}
    NoCopy(const NoCopy&) = delete;
    NoCopy& operator=(const NoCopy&) = delete;
};
struct NoCopyUser {
    NoCopy* p;
    NoCopyUser(NoCopy& n) : p(&n) { n.v += 1; }
    ~NoCopyUser() { p->v += 10; }
};

// `const int&` given an integer LITERAL: the argument has no address, so a temporary of the
// REFERENT's width must be materialized (a CFlat literal arrives narrower than `int`).
struct ConstIntRef {
    int seen;
    ConstIntRef(const int& x) : seen(x) {}
};

// A NON-const `int&` cannot bind a literal at all - Test/errors/err_cpp_ctor_ref_literal.cb.
struct IntRef {
    int* p;
    IntRef(int& x) : p(&x) { x += 1; }
    int get() const { return *p; }
};

struct LRef {
    int seen;
    LRef(Cnt& c, int& x) : seen(c.v + x) { c.v += 10; x += 20; }
};

// Controls: a free and a member function with the same `T&` parameter already passed the
// address, so these pin that the fix did not disturb them.
inline int by_ref_free(Cnt& c) { c.v += 100; return c.v; }
struct Mem { int bump(Cnt& c) { c.v += 1000; return c.v; } };

inline Cnt make_cnt(int v) { Cnt c; c.v = v; return c; }
inline int make_int(int v) { return v; }
inline std::string make_string() { return std::string("call"); }

struct RRefPair {
    int seen;
    RRefPair(Cnt&& c, int&& x) : seen(c.v + x + 100) { c.v += 10; x += 20; }
    ~RRefPair() {}
};

struct CtorChoice {
    inline static int copy_count = 0;
    inline static int move_count = 0;
    CtorChoice(const Cnt&) { ++copy_count; }
    CtorChoice(Cnt&&) { ++move_count; }
    static void reset() { copy_count = 0; move_count = 0; }
    static int copies() { return copy_count; }
    static int moves() { return move_count; }
};

struct ScalarRRef {
    int seen;
    ScalarRRef(int&& x) : seen(x + 1) { x += 10; }
    ~ScalarRRef() {}
};

struct PointerRRef {
    int seen;
    PointerRRef(int*&& p) : seen(*p) { *p += 10; }
    ~PointerRRef() {}
};

struct StringRRef {
    int seen;
    StringRRef(std::string&& s) : seen((int)s.size()) {}
    ~StringRRef() {}
};

template <typename T>
struct RRefBox {
    int seen;
    RRefBox(T&& x) : seen(x + 3) {}
};

// Reference-RETURNING helpers: their result is an LVALUE, so a `T&` ctor parameter must bind it.
inline int& pick_ref(int& a) { return a; }
struct RefBox {
    int v;
    RefBox() : v(0) {}
    int& ref() { return v; }
};

}
