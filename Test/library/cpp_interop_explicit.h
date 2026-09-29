// `explicit` on a C++ constructor, at a CFlat call argument. C++ offers exactly ONE implicit
// user-defined conversion at an argument and an `explicit` constructor is not a candidate for
// it; the spelled form `cppexp.Ex(5)` stays legal. The non-explicit `Options(double)` shapes
// below are the ACCEPT set: they must keep converting at every parameter kind C++ allows.
#pragma once
#include <memory>
#include <optional>
#include <type_traits>
#include <vector>

namespace cppexp {

struct Ex { int v; explicit Ex(int a) : v(a) {} };
inline int take_ex(Ex e) { return e.v; }
inline int take_ex_cref(const Ex& e) { return e.v; }
inline int take_ex_rref(Ex&& e) { return e.v; }
inline int take_ex_two(Ex e, int k) { return e.v + k; }

struct ExHost {
    int base;
    ExHost(int b) : base(b) {}
    int member_take(Ex e) const { return base + e.v; }
    static int static_take(Ex e) { return e.v * 10; }
};

// An explicit constructor whose SECOND parameter has a default: the one-argument call shape
// exists, and is still not an implicit conversion.
struct ExDef { int v; explicit ExDef(int a, int b = 7) : v(a + b) {} };
inline int take_exdef(ExDef e) { return e.v; }

// Both an explicit and a non-explicit constructor, of different arities.
struct Both {
    int v;
    explicit Both(int a) : v(a) {}
    Both(int a, int b) : v(a * 100 + b) {}
};
inline int take_both(Both b) { return b.v; }

// A free operator whose OPERAND would need the conversion.
inline int operator-(const Ex& a, const Ex& b) { return a.v - b.v; }

// A C++ CONSTRUCTOR parameter of class type.
struct ExOwner { int v; ExOwner(Ex e) : v(e.v + 1) {} };

// An explicit COPY constructor: copying an ExCopy must keep working.
struct ExCopy {
    int v;
    ExCopy(int a) : v(a) {}
    explicit ExCopy(const ExCopy& o) : v(o.v + 1000) {}
};
inline int take_excopy_cref(const ExCopy& c) { return c.v; }

// ACCEPT set - a NON-explicit converting constructor, offered at every parameter kind.
struct Options { double lr; Options(double l) : lr(l) {} };
inline int opt_by_value(Options o) { return (int)(o.lr * 100); }
inline int opt_by_cref(const Options& o) { return (int)(o.lr * 100) + 1; }
inline int opt_by_rref(Options&& o) { return (int)(o.lr * 100) + 2; }
inline int operator-(const Options& a, const Options& b) { return (int)((a.lr - b.lr) * 100); }
// A NON-const lvalue reference takes no user-defined conversion at all, free or member: the
// converted temporary has no address the callee could write through. A NAMED lvalue still binds.
inline int opt_by_lref(Options& o) { return (int)(o.lr * 100) + 3; }
struct OptHost {
    int base;
    OptHost(int b) : base(b) {}
    int member_lref(Options& o) const { return base + (int)(o.lr * 100); }
};
struct OptOwner { int v; OptOwner(Options o) : v((int)(o.lr * 100) + 7) {} };
struct Optim {
    int v;
    Optim(std::vector<int> params, Options o) : v((int)params.size() * 1000 + (int)(o.lr * 100)) {}
};
struct OptimCref {
    int v;
    OptimCref(std::vector<int> p, const Options& o) : v((int)p.size() * 2000 + (int)(o.lr * 100)) {}
};

// The libtorch Adam shape: the options class converts from a DEFAULTED double, and the owner
// takes it as a defaulted by-value parameter.
struct OptDef { double lr; OptDef(double l = 1e-3) : lr(l) {} };
struct OptimDef {
    int v;
    OptimDef(std::vector<int> p, OptDef o = {}) : v((int)p.size() * 3000 + (int)(o.lr * 1000)) {}
};
// Two overloads that both accept the same shortened call (libtorch's bernoulli_out): the call
// `amb_pick(o, s)` is ambiguous in C++, so its default wrapper cannot be built. The spelled-out
// calls and every other default wrapper in the header must still bind.
inline int amb_pick(int& out, int self, std::optional<int> g = std::nullopt)
{ return out + self + (g ? *g : 100); }
inline int amb_pick(int& out, int self, double p = 0.5, std::optional<int> g = std::nullopt)
{ return out + self + (int)(p * 1000) + (g ? *g : 200); }
inline int amb_sibling(int x, std::optional<int> g = std::nullopt) { return x + (g ? *g : 30); }
// A move-only by-value parameter ahead of a default: its wrapper must move the argument on,
// or the header's whole default-wrapper batch fails to compile.
inline int amb_moveonly(std::unique_ptr<int> p, std::optional<int> g = std::nullopt)
{ return p ? *p : -1; }
struct AmbHost {
    int base;
    AmbHost(int b) : base(b) {}
    int pick(int a, std::optional<int> g = std::nullopt) const { return base + a + (g ? *g : 40); }
    int pick(int a, double p = 0.5, std::optional<int> g = std::nullopt) const
    { return base + a + (int)(p * 1000) + (g ? *g : 50); }
    int sibling(int a, std::optional<int> g = std::nullopt) const { return base + a + (g ? *g : 60); }
};

// Overload-ranking probes for converting constructors against standard conversions.
struct MemRankHost {
    int pick(int, std::optional<int> = std::nullopt) const { return 1; }
    int pick(int, double = 0.5, std::optional<int> = std::nullopt) const { return 3606; }
};
inline int memrank_free(int, std::optional<int> = std::nullopt) { return 1; }
inline int memrank_free(int, double = 0.5, std::optional<int> = std::nullopt) { return 3607; }
struct MemRankNoTail {
    int pick(int, std::optional<int> = std::nullopt) const { return 1; }
    int pick(int, double = 0.5) const { return 3608; }
};
struct MemRankLong {
    int pick(int, std::optional<int>) const { return 1; }
    int pick(int, long) const { return 3609; }
};
struct MemRankOptionalDouble {
    int pick(int, std::optional<double>) const { return 1; }
    int pick(int, float) const { return 3610; }
};
struct MemRankOpt { MemRankOpt(double) {} };
struct MemRankNonTemplate {
    int pick(int, MemRankOpt) const { return 1; }
    int pick(int, double) const { return 3611; }
};
// A converting-constructor temporary binds `T&&` over `const T&` ([over.ics.rank] 3.2.3).
struct MemRankBox { int v; MemRankBox(int x) : v(x) {} };
struct MemRankRref {
    int n = 0;
    void push(MemRankBox&& b) { n = 3700 + b.v; }
    void push(const MemRankBox& b) { n = 1; }
};
inline int memrank_rref(MemRankBox&& b) { return 3700 + b.v; }
inline int memrank_rref(const MemRankBox& b) { return 1; }

struct LateMemberWrapper {
    int add(int n = 9) const { return n + 1; }
};

// The libtorch DataLoader shape: a class template's field specializes another template over its
// PROTECTED nested type, and both carry defaulted members. No default-argument wrapper can spell
// `PrivLoader<int>::Job` at namespace scope; an earlier field's wrapper must still bind.
template <typename T> struct PrivQueue {
    int ahead(std::optional<int> g = std::nullopt) const { return g ? *g : 3; }
    T slot{};
    T pop(std::optional<int> timeout = std::nullopt) { return slot; }
};
template <typename T> struct PrivBox {
    T v{};
    int get(std::optional<int> g = std::nullopt) const { return g ? *g : 9; }
};
template <typename D> class PrivLoader {
protected:
    struct Job { int id = 7; };
public:
    PrivBox<D> box;
    PrivQueue<Job> jobs;
    int peek(std::optional<int> g = std::nullopt) const { return g ? *g : 5; }
    int next() { return jobs.pop().id + (int)sizeof(D); }
};

struct NewPlain {
    int v;
    int how;
    NewPlain(int a, int b) : v(a + b), how(9) {}
};

struct NewTemplate {
    int v;
    int how;
    template<class T> NewTemplate(T x) : v((int)x + 20), how(1) {}
};

struct NewReq {
    int v;
    int how;
    template<class T> requires (sizeof(T) == 8)
    NewReq(T x) : v((int)x + 100), how(2) {}
};

struct NewEnabled {
    int v;
    int how;
    template<class T, std::enable_if_t<std::is_same_v<T, double>, int> = 0>
    NewEnabled(T x) : v((int)(x * 10.0)), how(3) {}
};

struct NewDefault {
    int v;
    int how;
    NewDefault() : v(41), how(4) {}
};

struct NewInner {
    int v;
    NewInner(int x) : v(x + 50) {}
};

struct NewWithMember {
    NewInner inner;
    int how;
    NewWithMember(int x) : inner(x), how(5) {}
};

struct NewDeleted {
    NewDeleted(int) = delete;
};

struct NewNarrow {
    int v;
    NewNarrow(int x) : v(x) {}
};

// Converting constructors at the copy-initialization positions (init, assignment, return, field,
// element, brace element): `T t = u;` means `T t = T(u);` (plan converting-constructors.md).
// ConvCnt counts constructions and destructions so a double destroy or a leak shows.
inline int conv_ctors = 0;
inline int conv_dtors = 0;
inline void conv_reset() { conv_ctors = 0; conv_dtors = 0; }
inline int conv_ctor_count() { return conv_ctors; }
inline int conv_dtor_count() { return conv_dtors; }
struct ConvCnt {
    int v = 0;
    ConvCnt() { ++conv_ctors; }
    ConvCnt(int x) : v(x) { ++conv_ctors; }
    ConvCnt(const ConvCnt& o) : v(o.v + 100) { ++conv_ctors; }
    ConvCnt(ConvCnt&& o) : v(o.v + 1000) { ++conv_ctors; }
    ConvCnt& operator=(const ConvCnt& o) { v = o.v + 10000; return *this; }
    ConvCnt& operator=(ConvCnt&& o) { v = o.v + 20000; return *this; }
    ~ConvCnt() { ++conv_dtors; }
};
// A `const char*` converting constructor: a string literal passes its constant pointer.
struct ConvStr {
    const char* p = nullptr;
    int n = 0;
    ConvStr() = default;
    ConvStr(const char* s) : p(s) { while (s[n]) ++n; }
    ConvStr(const ConvStr& o) : p(o.p), n(o.n) {}
    ConvStr& operator=(const ConvStr& o) { p = o.p; n = o.n + 100; return *this; }
    ~ConvStr() {}
};
// A converting constructor TEMPLATE plus `operator=(ConvVal)` taking its parameter BY VALUE.
struct ConvVal {
    long v = 0;
    ConvVal() = default;
    template <class T> ConvVal(T x) : v((long)x * 10) {}
    ConvVal(const ConvVal& o) : v(o.v + 1) {}
    ConvVal& operator=(ConvVal o) { v = o.v; return *this; }
    ~ConvVal() {}
};
// A direct `operator=(int)` wins over building ConvDirect(int).
struct ConvDirect {
    int v = 0;
    int via = 0;
    int calls = 0;
    ConvDirect() = default;
    ConvDirect(int x) : v(x), via(1) {}
    ConvDirect(const ConvDirect& o) : v(o.v), via(o.via) {}
    ConvDirect& operator=(const ConvDirect& o) { v = o.v; via = 2; return *this; }
    ConvDirect& operator=(int x) { v = x; via = 3; ++calls; return *this; }
    ~ConvDirect() {}
};
// The same with a noexcept `operator=(int)`: the call stays in one block (no invoke split).
struct ConvDirectNx {
    int v = 0;
    int via = 0;
    int calls = 0;
    ConvDirectNx() = default;
    ConvDirectNx(int x) noexcept : v(x), via(1) {}
    ConvDirectNx& operator=(const ConvDirectNx& o) noexcept { v = o.v; via = 2; return *this; }
    ConvDirectNx& operator=(int x) noexcept { v = x; via = 3; ++calls; return *this; }
};
struct AmbiguousDefault {
    int v;
    AmbiguousDefault() : v(19) {}
    AmbiguousDefault(int x = 5) : v(x) {}
};
struct InheritedDefaultBase {
    int v;
    InheritedDefaultBase(int x = 25) : v(x) {}
};
struct InheritedDefault : InheritedDefaultBase {
    using InheritedDefaultBase::InheritedDefaultBase;
    int w = 3;
};
class PrivateDefault {
public:
    int get() const { return 20; }
private:
    PrivateDefault() = default;
};
// Refusal shapes: explicit, two equal-rank converting constructors, and a chain of two
// user-defined conversions (const char* -> ConvStr -> ConvChain).
struct ConvExplicit { int v = 0; ConvExplicit() = default; explicit ConvExplicit(int a) : v(a) {} ~ConvExplicit() {} };
struct ConvAmb { int v = 0; ConvAmb() = default; ConvAmb(long) : v(1) {} ConvAmb(unsigned long) : v(2) {} ~ConvAmb() {} };
struct ConvChain { int n = 0; ConvChain() = default; ConvChain(ConvStr s) : n(s.n) {} ~ConvChain() {} };
// A TEMPLATE `operator=(U)` is an exact match for any source, so it beats T1(int) + copy.
struct ConvTplAssign {
    long v = 0;
    int via = 0;
    ConvTplAssign() = default;
    ConvTplAssign(int x) : v(x), via(1) {}
    ConvTplAssign(const ConvTplAssign& o) : v(o.v), via(o.via) {}
    ConvTplAssign& operator=(const ConvTplAssign& o) { v = o.v; via = 2; return *this; }
    template <class U> ConvTplAssign& operator=(U u) { v = (long)u * 3; via = 3; return *this; }
    ~ConvTplAssign() {}
};
// `operator=(double)` is what C++ calls for an int source (standard conversion); CFlat's call
// rules refuse int -> double, so the int source is refused rather than silently rerouted.
struct ConvDblAssign {
    long v = 0;
    int via = 0;
    ConvDblAssign() = default;
    ConvDblAssign(int x) : v(x), via(1) {}
    ConvDblAssign(const ConvDblAssign& o) : v(o.v), via(o.via) {}
    ConvDblAssign& operator=(const ConvDblAssign& o) { v = o.v; via = 2; return *this; }
    ConvDblAssign& operator=(double d) { v = (long)(d * 2); via = 3; return *this; }
    ~ConvDblAssign() {}
};
// Next to a template `operator=(U)`, C++ picks operator=<int> for an int source (an exact
// match) over `operator=(double)`; that is what runs, no refusal.
struct ConvDblTplAssign {
    long v = 0;
    int via = 0;
    ConvDblTplAssign() = default;
    ConvDblTplAssign(int x) : v(x), via(1) {}
    ConvDblTplAssign(const ConvDblTplAssign& o) : v(o.v), via(o.via) {}
    ConvDblTplAssign& operator=(const ConvDblTplAssign& o) { v = o.v; via = 2; return *this; }
    ConvDblTplAssign& operator=(double d) { v = (long)d; via = 3; return *this; }
    template <class U> ConvDblTplAssign& operator=(U u) { v = (long)u * 5; via = 5; return *this; }
    ~ConvDblTplAssign() {}
};
// `operator=(const char*)` is not viable for an int source, so C++ assigns through
// ConvPtrAssign(int) + copy-assign.
struct ConvPtrAssign {
    long v = 0;
    int via = 0;
    ConvPtrAssign() = default;
    ConvPtrAssign(int x) : v(x), via(1) {}
    ConvPtrAssign(const ConvPtrAssign& o) : v(o.v), via(o.via) {}
    ConvPtrAssign& operator=(const ConvPtrAssign& o) { v = o.v * 10; via = 2; return *this; }
    ConvPtrAssign& operator=(const char* s) { v = 0; via = 3; return *this; }
    ~ConvPtrAssign() {}
};
// `operator=(int&)` binds an int lvalue only; an rvalue int falls back to ConvRefAssign(int) +
// assignment. ConvRefLongAssign adds `operator=(long)`, which C++ picks for an rvalue.
struct ConvRefAssign {
    long v = 0;
    int via = 0;
    ConvRefAssign() {}
    ConvRefAssign(int x) : v(x), via(1) {}
    ConvRefAssign& operator=(const ConvRefAssign& o) { v = o.v; via = 2; return *this; }
    ConvRefAssign& operator=(int& x) { v = x; via = 6; return *this; }
    ConvRefAssign& operator+=(int& x) { v += x; via = 8; return *this; }
};
struct ConvRefLongAssign {
    long v = 0;
    int via = 0;
    ConvRefLongAssign() {}
    ConvRefLongAssign(int x) : v(x), via(1) {}
    ConvRefLongAssign(const ConvRefLongAssign& o) : v(o.v), via(o.via) {}
    ConvRefLongAssign& operator=(const ConvRefLongAssign& o) { v = o.v; via = 2; return *this; }
    ConvRefLongAssign& operator=(int& x) { v = x; via = 6; return *this; }
    ConvRefLongAssign& operator=(long x) { v = x; via = 7; return *this; }
    ~ConvRefLongAssign() {}
};
// Class-to-class: a moved source binds ConvDstB(ConvSrcA&&) and is modified IN PLACE.
struct ConvSrcA {
    int v = 0;
    ConvSrcA() { ++conv_ctors; }
    ConvSrcA(int x) : v(x) { ++conv_ctors; }
    ConvSrcA(const ConvSrcA& o) : v(o.v) { ++conv_ctors; }
    ConvSrcA(ConvSrcA&& o) : v(o.v) { o.v = -1; ++conv_ctors; }
    ~ConvSrcA() { ++conv_dtors; }
};
struct ConvDstB {
    int v = 0;
    int via = 0;
    ConvDstB() { ++conv_ctors; }
    ConvDstB(const ConvSrcA& a) : v(a.v), via(1) { ++conv_ctors; }
    ConvDstB(ConvSrcA&& a) : v(a.v), via(2) { a.v = -7; ++conv_ctors; }
    ConvDstB(const ConvDstB& o) : v(o.v), via(o.via) { ++conv_ctors; }
    ConvDstB(ConvDstB&& o) : v(o.v), via(o.via) { ++conv_ctors; }
    ConvDstB& operator=(const ConvDstB& o) { v = o.v; via = o.via + 10; return *this; }
    ConvDstB& operator=(ConvDstB&& o) { v = o.v; via = o.via + 20; return *this; }
    ~ConvDstB() { ++conv_dtors; }
};
inline int conv_take_dst(ConvDstB b) { return b.via; }
// A pointer source into a constructor taking a pointer to a non-class pointee.
struct ConvPtr { const int* p = nullptr; int set = 0; ConvPtr() = default; ConvPtr(const int* q) : p(q), set(1) {} ~ConvPtr() {} };
// A pointer ARGUMENT into a class parameter whose converting constructor takes it.
struct ConvIp { int v = 0; ConvIp(const int* p) : v(*p) {} };
struct ConvIpB { int v = 0; ConvIpB(const int* p) : v(*p) {} };
struct ConvIpE { int v = 0; explicit ConvIpE(const int* p) : v(*p) {} };
struct ConvCp { int n = 0; ConvCp(const char* s) { while (s[n]) ++n; } };
inline int conv_ip_val(ConvIp c) { return c.v + 100; }
inline int conv_ip_two(ConvIp a, ConvIp b) { return a.v * 10 + b.v; }
inline int conv_ip_cref(const ConvPtr& c) { return c.set * 100 + *c.p; }
inline int conv_ip_exact(ConvIp c) { return 1; }
inline int conv_ip_exact(const int* p) { return 2; }
inline int conv_ip_amb(ConvIp c) { return 1; }
inline int conv_ip_amb(ConvIpB c) { return 2; }
inline int conv_ip_explicit(ConvIpE c) { return c.v; }
inline int conv_cp_val(ConvCp c) { return c.n; }
// A `bool` sibling: C++ takes pointer -> bool (standard) over ConvIp(const int*) (user-defined).
inline int conv_ip_bool(ConvIp c) { return 1; }
inline int conv_ip_bool(bool b) { return 2; }
inline int conv_ip_bool_cref(const ConvPtr& c) { return 1; }
inline int conv_ip_bool_cref(bool b) { return 2; }
// The bool sibling is NOT viable on its second argument, so C++ calls the ConvIp overload.
struct ConvIpOther { int v = 0; };
inline int conv_ip_oa(ConvIp c, int n) { return c.v + n; }
inline int conv_ip_oa(bool b, ConvIpOther* o) { return 99; }
struct ConvIpHost {
    int base = 1000;
    ConvIpHost() = default;
    ConvIpHost(ConvIp c) : base(c.v) {}
    int add(ConvIp c) const { return base + c.v; }
    int operator-(ConvIp c) const { return base - c.v; }
    int pick(ConvIp c) const { return 1; }
    int pick(bool b) const { return 2; }
};
template <class T> struct ConvBox {
    T v{};
    int tag = 0;
    ConvBox() = default;
    ConvBox(T x) : v(x), tag(1) {}
    ConvBox(const ConvBox& o) : v(o.v), tag(o.tag) {}
    ConvBox& operator=(const ConvBox& o) { v = o.v; tag = o.tag + 10; return *this; }
    ~ConvBox() {}
};

// Explicit full specializations are harvested by the general record walk. Keep their canonical
// identities distinct so members and by-value boundaries cannot collapse onto the primary name.
namespace cppresult {
struct First {};
struct Middle {};
struct Last {};
template <class T> struct Result;
template <> struct Result<Last> { int value; Result() : value(303) {} int get() const { return value; } };
template <> struct Result<Middle> { int value; Result() : value(202) {} int get() const { return value; } };
template <> struct Result<First> { int value; Result() : value(101) {} int get() const { return value; } };
struct Factory { Result<Middle> middle() const { return Result<Middle>(); } };
inline Result<First> first() { return Result<First>(); }
inline Result<Last> last() { return Result<Last>(); }
inline int take_first(Result<First> value) { return value.get(); }
inline int take_middle(Result<Middle> value) { return value.get(); }
inline int take_last(Result<Last> value) { return value.get(); }
}
}
