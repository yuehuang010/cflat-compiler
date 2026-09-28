#pragma once
#include <string>
#include <cstddef>
#include <cstdint>
#include <sys/types.h>
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

// Same-canonical-type typedef spellings must bind and mutate caller storage.
struct SSizeRef { ssize_t* p; SSizeRef(ssize_t& x) : p(&x) { ++x; } };
struct PDiffRef { ptrdiff_t* p; PDiffRef(ptrdiff_t& x) : p(&x) { ++x; } };
struct SizeRef { size_t* p; SizeRef(size_t& x) : p(&x) { ++x; } };
struct Int64Ref { std::int64_t* p; Int64Ref(std::int64_t& x) : p(&x) { ++x; } };
namespace n {
using I = long;
struct IRef { I* p; IRef(I& x) : p(&x) { ++x; } };
}

struct ConstSignedCharRef {
    const signed char* p;
    signed char seen;
    ConstSignedCharRef(const signed char& x) : p(&x), seen(x) {}
    bool same(const void* q) const { return p == q; }
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
struct SignedCharRef { SignedCharRef(signed char& x) { ++x; } };
struct LongRef { LongRef(long& x) { ++x; } };
struct RefOverload {
    int chosen;
    RefOverload(signed char&) : chosen(1) {}
    RefOverload(char&) : chosen(2) {}
};
// `long&` listed first: a literal or an `int` lvalue must make it non-viable, not refuse early.
struct RefConstOverload {
    int chosen;
    long value;
    RefConstOverload(long& x) : chosen(1), value(x) { x += 10; }
    RefConstOverload(const long& x) : chosen(2), value(x) {}
};
// Forwarded exact types let clang rank these as at the source site.
struct RvalueOrConst { int chosen; long v; RvalueOrConst(unsigned&& r) : chosen(1), v(r) { r = 99; } RvalueOrConst(const long& r) : chosen(2), v(r) {} };
// A converted pick keeps the C++ value: bool is 0/1, double -> unsigned is fptoui.
struct ThkDbl { double v; ThkDbl(const double& x) : v(x) {} };
struct ThkUns { unsigned v; ThkUns(const unsigned& x) : v(x) {} };
struct LongLongOrDouble { int chosen; long long v; LongLongOrDouble(long long& r) : chosen(1), v(r) { r = 99; } LongLongOrDouble(double d) : chosen(2), v((long long)d) {} };
struct ExprLongRef { int v; ExprLongRef(long& r) : v((int)r) { r = 99; } };
struct LongLongRefOnly { int v; LongLongRefOnly(long long& r) : v((int)r) { r = 99; } };
// The clang-resolved mirror is faithful to the candidate set: a constructor template, a
// `...` constructor, and a deleted / private / protected constructor all take part exactly
// as at a C++ call site.
struct TplOrConst { int chosen; TplOrConst(const long& x) : chosen(1) {} template <class T> TplOrConst(T&&) : chosen(2) {} };
struct VarOrConst { int chosen; VarOrConst(const long& x) : chosen(1) {} VarOrConst(int, ...) : chosen(2) {} };
struct DelOrConst { int chosen; DelOrConst(const long& x) : chosen(1) {} DelOrConst(int&) = delete; };
class PrivOrConst { public: int chosen; PrivOrConst(const long& x) : chosen(1) {} private: PrivOrConst(int&) : chosen(2) {} };
class ProtOrConst { public: int chosen; ProtOrConst(const long& x) : chosen(1) {} protected: ProtOrConst(int&) : chosen(2) {} };
// A cast result is a prvalue: `long&` must not bind (nor write) the operand's slot.
struct CastWrite { int chosen; long seen; CastWrite(long& x) : chosen(1), seen(x) { x = 99; } CastWrite(const long& x) : chosen(2), seen(x) {} };
struct SignedCharBox { SignedCharBox(signed char& x) { ++x; } };
struct OrdinarySignedCharCall { int take(SignedCharBox) const { return 1; } };

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
inline int* make_ptr(int* p) { return p; }
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

struct RefKindPtrApi {
    int member_c(int* const& p) const { return p == nullptr ? 100 : *p + 100; }
    int member_r(int*&& p) const { if (p != nullptr) *p += 10; return p == nullptr ? 200 : *p + 200; }
    static int static_c(int* const& p) { return p == nullptr ? 100 : *p + 100; }
    static int static_r(int*&& p) { if (p != nullptr) *p += 10; return p == nullptr ? 200 : *p + 200; }
};

inline int refkind_ptr_r(int*&& p)
{
    if (p != nullptr) *p += 10;
    return p == nullptr ? 200 : *p + 200;
}

inline int refkind_ptrptr_r(int**&& p)
{
    return p == nullptr || *p == nullptr ? 300 : **p + 300;
}

struct RefKindScalarApi {
    int member_l(int& x) const { return ++x; }
    int member_r(int&& x) const { return x + 200; }
    static int static_l(int& x) { return ++x; }
    static int static_r(int&& x) { return x + 200; }
};

inline int refkind_int_l(int& x) { return ++x; }
inline int refkind_int_r(int&& x) { return x + 200; }

}

// Section M103: a scalar converted to a C++ class through the generated ctor thunk (taken when
// the class has its own ctor TEMPLATE) must bind a `const T&` / `T&&` view to CALLER storage.
// The thunk used to take the scalar by value, so the view pointed at a dead thunk slot;
// thk_clobber() in the callee overwrites that stack so a stale read cannot pass by luck.
#include <cstddef>
namespace cppcr {
__attribute__((noinline)) inline void thk_clobber()
{
    volatile long junk[64];
    for (int i = 0; i < 64; ++i) junk[i] = -7;
}
template <class T> struct ThkBase {
    const T* Data; size_t Length;
    ThkBase(const T& one) : Data(&one), Length(1) {}
    ThkBase(const T* d, size_t c) : Data(d), Length(c) {}
};
// Inherited `const T&` ctor + own ctor template: the c10::ArrayRef shape.
template <class T> struct ThkArr : ThkBase<T> {
    using ThkBase<T>::ThkBase;
    template <size_t N> ThkArr(const T (&a)[N]) : ThkBase<T>(a, N) {}
    T first() const { return this->Data[0]; }
};
// Plain class, own `const long&` view ctor.
struct ThkView {
    const long* d;
    ThkView(const long& x) : d(&x) {}
    template <size_t N> ThkView(const long (&a)[N]) : d(a) {}
    long first() const { return *d; }
};
// `long&&` view ctor.
struct ThkRView {
    const long* d;
    ThkRView(long&& x) : d(&x) {}
    template <size_t N> ThkRView(const long (&a)[N]) : d(a) {}
    long first() const { return *d; }
};
// Copies from the reference: must still see the value.
struct ThkCopy {
    long v;
    ThkCopy(const long& x) : v(x) {}
    template <size_t N> ThkCopy(const long (&a)[N]) : v(a[0]) {}
    long first() const { return v; }
};
// Non-const `long&`: a named lvalue binds its own slot, so writes reach it.
struct ThkLRef {
    long* d;
    ThkLRef(long& x) : d(&x) {}
    template <size_t N> ThkLRef(long (&a)[N]) : d(a) {}
    long first() const { return *d; }
};
// Only a ctor TEMPLATE (no listed scalar ctor): the thunk forwards the argument's own type.
struct ThkAny {
    const void* p;
    long (*get)(const void*);
    template <class V> ThkAny(const V& v)
        : p(&v), get([](const void* q) { return (long)*static_cast<const V*>(q); }) {}
    long first() const { return get(p); }
};
struct ThkMutLL {
    long v;
    ThkMutLL(long long&& x) : v((long)x) { x += 100; }
    template <size_t N> ThkMutLL(const long long (&a)[N]) : v((long)a[0]) {}
};
struct ThkMutU {
    unsigned v;
    ThkMutU(unsigned&& x) : v(x) { x += 100; }
    template <size_t N> ThkMutU(const unsigned (&a)[N]) : v(a[0]) {}
};
struct ThkMutI {
    int v;
    ThkMutI(int&& x) : v(x) { x += 100; }
    template <size_t N> ThkMutI(const int (&a)[N]) : v(a[0]) {}
};
struct ThkAddrLL {
    const void* p;
    ThkAddrLL(const long long& x) : p(&x) {}
    template <size_t N> ThkAddrLL(const long long (&a)[N]) : p(a) {}
};
struct ThkAddrI {
    const void* p;
    ThkAddrI(const int& x) : p(&x) {}
    template <size_t N> ThkAddrI(const int (&a)[N]) : p(a) {}
};
struct PlainConstLLRef {
    const void* p;
    PlainConstLLRef(const long long& x) : p(&x) {}
    int same(const void* other) const { return p == other; }
};
struct ThkUser {
    ThkUser(int) {}
    long arr(ThkArr<long> a) const { thk_clobber(); return a.first(); }
    double darr(ThkArr<double> a) const { thk_clobber(); return a.first(); }
    long view(ThkView a) const { thk_clobber(); return a.first(); }
    long rview(ThkRView a) const { thk_clobber(); return a.first(); }
    long copy(ThkCopy a) const { thk_clobber(); return a.first(); }
    long any(ThkAny a) const { thk_clobber(); return a.first(); }
    long lref(ThkLRef a) const { *a.d += 100; thk_clobber(); return a.first(); }
    long mutll(ThkMutLL a) const { return a.v; }
    long mutu(ThkMutU a) const { return a.v; }
    long muti(ThkMutI a) const { return a.v; }
    int addrll(ThkAddrLL a, const void* p) const { return a.p == p; }
    int addri(ThkAddrI a, const void* p) const { return a.p == p; }
};
}

// A braced list reaching a C++ constructor through the generated thunk is backed by CALLER
// storage, as C++ keeps it to the end of the caller's full-expression. The thunk used to build
// the array / initializer_list itself, so a kept view dangled; each reader overwrites its OWN
// frame (the depth the thunk ran at) before reading.
#include <initializer_list>
namespace cppcr {
struct BrcIl {
    const long* d; size_t n;
    BrcIl(std::initializer_list<long> l) : d(l.begin()), n(l.size()) {}
    ~BrcIl() {}
};
struct BrcIlD {
    const double* d;
    BrcIlD(const std::initializer_list<double>& l) : d(l.begin()) {}
    ~BrcIlD() {}
};
struct BrcTwo {
    const long* d; int k;
    BrcTwo(int k_, const long (&a)[2]) : d(a), k(k_) {}
};
// Overloads differing only in element type: clang picks by the list's own element types.
struct BrcPickA {
    const void* p; int which;
    BrcPickA(const int (&a)[2]) : p(a), which(1) {}
    BrcPickA(const long (&a)[2]) : p(a), which(2) {}
};
struct BrcPickL {
    const void* p; int which;
    BrcPickL(std::initializer_list<int> l) : p(l.begin()), which(1) {}
    BrcPickL(std::initializer_list<double> l) : p(l.begin()), which(2) {}
    ~BrcPickL() {}
};
// The other constructor arguments take part in ranking: (long, {1}) picks the second.
struct BrcRank {
    int which; long first;
    BrcRank(int, std::initializer_list<int> l) : which(1), first(*l.begin()) {}
    BrcRank(long, std::initializer_list<const int> l) : which(2), first(*l.begin()) {}
    ~BrcRank() {}
};
// A string literal / nullptr beside the list ranks like the C++ argument it is.
struct BrcRankS {
    int which;
    BrcRankS(const char*, std::initializer_list<int>) : which(1) {}
    BrcRankS(const char*, std::initializer_list<double>) : which(2) {}
    ~BrcRankS() {}
};
// A constructor template beside the list constructor: clang deduces and ranks it too.
template <class T> struct BrcTpl {
    const void* p; int which;
    template <class U> BrcTpl(U, std::initializer_list<T> l) : p(l.begin()), which(1) {}
    BrcTpl(int, std::initializer_list<double> l) : p(l.begin()), which(2) {}
    ~BrcTpl() {}
    long probe() const {
        volatile long junk[64];
        for (int i = 0; i < 64; ++i) junk[i] = -7;
        return which == 1 ? (long)((const T*)p)[1] * 10 + 1 : (long)((const double*)p)[1] * 10 + 2;
    }
};
__attribute__((noinline)) inline long brc_picka(BrcPickA a)
{
    volatile long junk[64];
    for (int i = 0; i < 64; ++i) junk[i] = -7;
    return a.which == 1 ? ((const int*)a.p)[1] * 10 + 1 : ((const long*)a.p)[1] * 10 + 2;
}
__attribute__((noinline)) inline double brc_pickl(const BrcPickL& a)
{
    volatile long junk[64];
    for (int i = 0; i < 64; ++i) junk[i] = -7;
    return a.which == 1 ? ((const int*)a.p)[1] * 10 + 1 : ((const double*)a.p)[1] * 10 + 2;
}
__attribute__((noinline)) inline long brc_il(const BrcIl& a)
{
    volatile long junk[64];
    for (int i = 0; i < 64; ++i) junk[i] = -7;
    return a.d[a.n - 1] * 10 + (long)a.n;
}
__attribute__((noinline)) inline double brc_ild(const BrcIlD& a)
{
    volatile long junk[64];
    for (int i = 0; i < 64; ++i) junk[i] = -7;
    return a.d[1];
}
__attribute__((noinline)) inline long brc_two(BrcTwo a)
{
    volatile long junk[64];
    for (int i = 0; i < 64; ++i) junk[i] = -7;
    return a.d[0] * 10 + a.d[1] + a.k;
}
}
