#pragma once

namespace ternref {
struct C { int v; };
struct Derived : C { int tag; };

inline C& ref_a() { static C a{1}; return a; }
inline C& ref_b() { static C b{2}; return b; }
inline C& ref_c() { static C c{3}; return c; }
inline Derived& ref_derived() { static Derived d{{4}, 40}; return d; }
inline C value_a() { return C{5}; }
inline C value_b() { return C{6}; }
inline void write(C* p, int value) { p->v = value; }
// T391: a counted class for `c ? default : x` arguments to C++ free functions and members.
inline int* t391_counts() { static int c[5] = { 0, 0, 0, 0, 0 }; return c; }
inline void t391_reset() { for (int i = 0; i < 5; ++i) t391_counts()[i] = 0; }
// ctor, copy, move, dtor, live allocations, packed one decimal digit each.
inline int t391_stats()
{
    int* c = t391_counts();
    return c[0] * 10000 + c[1] * 1000 + c[2] * 100 + c[3] * 10 + c[4];
}
struct T391Val
{
    int* p;
    T391Val() : p(new int(0)) { ++t391_counts()[0]; ++t391_counts()[4]; }
    explicit T391Val(int n) : p(new int(n)) { ++t391_counts()[0]; ++t391_counts()[4]; }
    T391Val(const T391Val& o) : p(new int(*o.p)) { ++t391_counts()[1]; ++t391_counts()[4]; }
    T391Val(T391Val&& o) noexcept : p(o.p) { o.p = nullptr; ++t391_counts()[2]; }
    ~T391Val() { if (p != nullptr) --t391_counts()[4]; delete p; ++t391_counts()[3]; }
    int value() const { return p != nullptr ? *p : -1; }
};
// Converts to T391Val through a user conversion operator (a different class arm).
struct T391Zero
{
    int field;
    T391Zero() : field(0) {}
    operator T391Val() const { return T391Val(); }
};
inline int t391_take(T391Val v) { return v.value(); }
inline int t391_tref(const T391Val& v) { return v.value(); }
struct T391Host
{
    int tref(const T391Val& v) { return 30 + v.value(); }
    int take(T391Val v) { return 40 + v.value(); }
};
// T459: static member and constructor parameters taking `const T391Val&`.
struct T391StaticHost
{
    static int tref(const T391Val& v) { return 50 + v.value(); }
};
struct T391Sink
{
    int stored;
    explicit T391Sink(const T391Val& v) : stored(v.value()) {}
    int value() const { return stored; }
};
// T459 r3: a const T391Val& constructor beside an int one; `c ? default : int local` selects the int one.
struct T391Mixed
{
    int x;
    T391Mixed(const T391Val& v) : x(10 + v.value()) {}
    T391Mixed(int n) : x(20 + n) {}
    int value() const { return x; }
};
}

struct T513CppA
{
    int value;
    T513CppA() noexcept : value(1) {}
    ~T513CppA() noexcept {}
};
struct T513CppB
{
    int value;
    T513CppB() noexcept : value(2) {}
    ~T513CppB() noexcept {}
};
inline T513CppA t513MakeA() noexcept { return T513CppA(); }
inline T513CppB t513MakeB() noexcept { return T513CppB(); }

inline int t513CtorFromA = 0;
inline int t513CtorDefault = 0;
inline int t513CtorLast = 0;
inline void t513ResetCtorStats() { t513CtorFromA = 0; t513CtorDefault = 0; t513CtorLast = 0; }
inline int t513CtorStats() noexcept { return t513CtorFromA * 1000 + t513CtorDefault * 100 + t513CtorLast; }
struct T513CppC
{
    int value;
    T513CppC() noexcept : value(2) { ++t513CtorDefault; t513CtorLast = value; }
    T513CppC(const T513CppA& a) noexcept : value(a.value + 10) { ++t513CtorFromA; t513CtorLast = value; }
    ~T513CppC() noexcept {}
};
inline T513CppC t513MakeC() noexcept { return T513CppC(); }

inline int t513OperatorCalls = 0;
inline int t513OperatorLast = 0;
inline void t513ResetOperatorStats() { t513OperatorCalls = 0; t513OperatorLast = 0; }
inline int t513OperatorStats() noexcept { return t513OperatorCalls * 10 + t513OperatorLast; }
struct T513CppOpTarget
{
    int value;
    T513CppOpTarget(int v = 2) noexcept : value(v) {}
    ~T513CppOpTarget() noexcept {}
};
struct T513CppOp
{
    int value;
    operator T513CppOpTarget() const noexcept;
    ~T513CppOp() noexcept {}
};
inline T513CppOp::operator T513CppOpTarget() const noexcept
{
    ++t513OperatorCalls;
    t513OperatorLast = value + 20;
    return T513CppOpTarget(value + 20);
}
inline T513CppOp t513MakeOp() noexcept { return T513CppOp{ 3 }; }
inline T513CppOpTarget t513MakeOpTarget() noexcept { return T513CppOpTarget(2); }
struct T513CppE
{
    int value;
    T513CppE() noexcept : value(7) {}
    T513CppE(int v) noexcept : value(v) {}
    ~T513CppE() noexcept {}
};
inline T513CppE t513MakeE() noexcept { return T513CppE(7); }

inline int t513BaseCopies = 0;
inline int t513BaseMoves = 0;
inline int t513BaseDtors = 0;
inline int t513DerivedDtors = 0;
inline int t513BaseLast = 0;
inline void t513ResetBaseStats()
{
    t513BaseCopies = 0;
    t513BaseMoves = 0;
    t513BaseDtors = 0;
    t513DerivedDtors = 0;
    t513BaseLast = 0;
}
inline int t513BaseStats() noexcept
{
    return t513BaseCopies * 10000 + t513BaseMoves * 1000 + t513BaseDtors * 100
        + t513DerivedDtors * 10 + t513BaseLast;
}
struct T513CppBase
{
    int value;
    T513CppBase(int v = 2) noexcept : value(v) {}
    T513CppBase(const T513CppBase& other) noexcept : value(other.value) { ++t513BaseCopies; }
    T513CppBase(T513CppBase&& other) noexcept : value(other.value) { ++t513BaseMoves; }
    ~T513CppBase() noexcept { ++t513BaseDtors; t513BaseLast = value; }
};
struct T513CppDerived : T513CppBase
{
    T513CppDerived(int v = 1) noexcept : T513CppBase(v) {}
    ~T513CppDerived() noexcept { ++t513DerivedDtors; }
};
inline T513CppBase t513MakeBase() noexcept { return T513CppBase(2); }
inline T513CppDerived t513MakeDerived() noexcept { return T513CppDerived(1); }

// T513 r4: a base with IMPLICIT copy/move ctors (only dtors are user-declared) and a derived over it.
inline int t513PlainDtors = 0;
inline void t513ResetPlainDtors() { t513PlainDtors = 0; }
inline int t513PlainStats() noexcept { return t513PlainDtors; }
struct T513PlainBase
{
    int value;
    T513PlainBase(int v = 3) noexcept : value(v) {}
    ~T513PlainBase() noexcept { ++t513PlainDtors; }
};
struct T513PlainDerived : T513PlainBase
{
    T513PlainDerived() noexcept : T513PlainBase(4) {}
    ~T513PlainDerived() noexcept { ++t513PlainDtors; }
};
inline T513PlainBase t513MakePlainBase() noexcept { return T513PlainBase(3); }
inline T513PlainDerived t513MakePlainDerived() noexcept { return T513PlainDerived(); }
// A converting constructor TEMPLATE: its argument deduces from any source class.
inline int t513TmplCtors = 0;
struct T513Tmpl
{
    int value;
    T513Tmpl() noexcept : value(2) {}
    template <class U> T513Tmpl(const U& u) noexcept : value(u.value + 200) { ++t513TmplCtors; }
    ~T513Tmpl() noexcept {}
};
inline T513Tmpl t513MakeTmpl() noexcept { return T513Tmpl(); }

// T513 r5: the join's direction is clang's implicit-conversion decision (defaulted template
// parameter, inherited constructor template, A&& constructor, undeducible template, two-way).
inline int t513R5Hits = 0;
inline void t513ResetR5() { t513R5Hits = 0; }
inline int t513R5Stats() noexcept { return t513R5Hits; }
struct T513R5DefA
{
    int v = 1;
    ~T513R5DefA() noexcept { t513R5Hits += 1; }
};
struct T513R5DefB
{
    double v = 2;
    T513R5DefB() noexcept {}
    template <int N = 0> T513R5DefB(const T513R5DefA&) noexcept { t513R5Hits += 100; }
    ~T513R5DefB() noexcept { t513R5Hits += 1000; }
};
inline T513R5DefA t513R5MakeDefA() noexcept { return T513R5DefA(); }
inline T513R5DefB t513R5MakeDefB() noexcept { return T513R5DefB(); }
struct T513R5InhA
{
    int v = 1;
    ~T513R5InhA() noexcept { t513R5Hits += 1; }
};
struct T513R5InhBase
{
    double v = 2;
    T513R5InhBase() noexcept {}
    template <class U> T513R5InhBase(const U&) noexcept { t513R5Hits += 100; }
    ~T513R5InhBase() noexcept { t513R5Hits += 1000; }
};
struct T513R5InhB : T513R5InhBase
{
    using T513R5InhBase::T513R5InhBase;
};
inline T513R5InhA t513R5MakeInhA() noexcept { return T513R5InhA(); }
inline T513R5InhB t513R5MakeInhB() noexcept { return T513R5InhB(); }
struct T513R5RvA
{
    int v = 1;
    ~T513R5RvA() noexcept { t513R5Hits += 1; }
};
struct T513R5RvB
{
    double v = 2;
    T513R5RvB() noexcept {}
    T513R5RvB(T513R5RvA&&) noexcept { t513R5Hits += 100; }
    ~T513R5RvB() noexcept { t513R5Hits += 1000; }
};
inline T513R5RvA t513R5MakeRvA() noexcept { return T513R5RvA(); }
inline T513R5RvB t513R5MakeRvB() noexcept { return T513R5RvB(); }
struct T513R5UndB;
struct T513R5UndA
{
    int v = 1;
    T513R5UndA() noexcept {}
    T513R5UndA(const T513R5UndB&) noexcept { t513R5Hits += 10; }
    ~T513R5UndA() noexcept { t513R5Hits += 1; }
};
struct T513R5UndB
{
    double v = 2;
    T513R5UndB() noexcept {}
    template <class U, class V> T513R5UndB(const U&) noexcept { t513R5Hits += 100; }
    ~T513R5UndB() noexcept { t513R5Hits += 1000; }
};
inline T513R5UndA t513R5MakeUndA() noexcept { return T513R5UndA(); }
inline T513R5UndB t513R5MakeUndB() noexcept { return T513R5UndB(); }
// Both directions viable (A(const B&) and a deducible B(const U&) ctor): clang calls it ambiguous.
struct T513R5AmbB;
struct T513R5AmbA
{
    int v = 1;
    T513R5AmbA() noexcept {}
    T513R5AmbA(const T513R5AmbB&) noexcept { t513R5Hits += 10; }
    ~T513R5AmbA() noexcept { t513R5Hits += 1; }
};
struct T513R5AmbB
{
    double v = 2;
    T513R5AmbB() noexcept {}
    template <class U> T513R5AmbB(const U&) noexcept { t513R5Hits += 100; }
    ~T513R5AmbB() noexcept { t513R5Hits += 1000; }
};
inline T513R5AmbA t513R5MakeAmbA() noexcept { return T513R5AmbA(); }
inline T513R5AmbB t513R5MakeAmbB() noexcept { return T513R5AmbB(); }

// T513 r6: clang's conditional-operator rules on the actual arm categories and cv.
inline int t513R6Hits = 0;
inline void t513ResetR6() { t513R6Hits = 0; }
inline int t513R6Stats() noexcept { return t513R6Hits; }
// A derived prvalue arm over a base prvalue arm: the base copy constructor is the conversion.
struct T513R6B
{
    double v = 2;
    T513R6B() noexcept {}
    T513R6B(const T513R6B&) noexcept { t513R6Hits += 10; }
    ~T513R6B() noexcept { t513R6Hits += 100; }
};
struct T513R6D : T513R6B
{
    double w = 2;
    T513R6D() noexcept {}
    T513R6D(const T513R6B&) noexcept { t513R6Hits += 1000; }
    ~T513R6D() noexcept { t513R6Hits += 10000; }
};
inline T513R6B t513R6MakeB() noexcept { return T513R6B(); }
inline T513R6D t513R6MakeD() noexcept { return T513R6D(); }
// Lvalue arms of two classes: the conversion operator yields a reference, so nothing is copied.
struct T513R6NoCopy
{
    double v = 2;
    T513R6NoCopy() noexcept {}
    T513R6NoCopy(const T513R6NoCopy&) = delete;
    ~T513R6NoCopy() noexcept { t513R6Hits += 100; }
};
inline T513R6NoCopy t513R6Shared;
struct T513R6Conv
{
    int v = 1;
    operator T513R6NoCopy&() noexcept { t513R6Hits += 10; return t513R6Shared; }
    ~T513R6Conv() noexcept { t513R6Hits += 1; }
};
inline T513R6Conv t513R6ConvObj;
inline T513R6Conv& t513R6GetConv() noexcept { return t513R6ConvObj; }
inline T513R6NoCopy& t513R6GetNoCopy() noexcept { return t513R6Shared; }
// A const prvalue arm selects the B(const K&&) overload, not B(K&&).
struct T513R6K
{
    int v = 1;
    ~T513R6K() noexcept { t513R6Hits += 1; }
};
struct T513R6KB
{
    double v = 2;
    T513R6KB() noexcept {}
    T513R6KB(T513R6K&&) noexcept { t513R6Hits += 10; }
    T513R6KB(const T513R6K&&) noexcept { t513R6Hits += 1000; }
    ~T513R6KB() noexcept { t513R6Hits += 100; }
};
inline const T513R6K t513R6MakeK() noexcept { return T513R6K(); }
inline T513R6KB t513R6MakeKB() noexcept { return T513R6KB(); }
// Refused by clang: a const prvalue (or const lvalue) arm cannot bind the only non-const constructor.
struct T513R6CK
{
    int v = 1;
    ~T513R6CK() noexcept {}
};
struct T513R6CB
{
    double v = 2;
    T513R6CB() noexcept {}
    T513R6CB(T513R6CK&&) noexcept {}
};
struct T513R6CL
{
    double v = 2;
    T513R6CL() noexcept {}
    T513R6CL(T513R6CK&) noexcept {}
};
inline const T513R6CK t513R6MakeCK() noexcept { return T513R6CK(); }
inline const T513R6CK& t513R6CkRef() noexcept { static const T513R6CK k{}; return k; }
inline T513R6CB t513R6MakeCB() noexcept { return T513R6CB(); }
inline T513R6CL t513R6MakeCL() noexcept { return T513R6CL(); }
// T513 r7: each arm keeps the category of its own expression (lvalue, xvalue or prvalue), in both orders.
inline int t513R7Hits = 0;
inline void t513ResetR7() { t513R7Hits = 0; }
inline int t513R7Stats() noexcept { return t513R7Hits; }
struct T513R7TA
{
    int v = 1;
};
struct T513R7NA
{
    int v = 1;
    ~T513R7NA() noexcept {}
};
struct T513R7B
{
    double v = 2;
    T513R7B() noexcept {}
    T513R7B(T513R7TA&&) noexcept { t513R7Hits += 10; }
    T513R7B(T513R7NA&&) noexcept { t513R7Hits += 10; }
    ~T513R7B() noexcept { t513R7Hits += 100; }
};
inline T513R7TA t513R7MakeTA() noexcept { return T513R7TA(); }
inline T513R7B t513R7MakeB() noexcept { return T513R7B(); }
// Refused by clang: a trivial prvalue arm cannot bind the only constructor, B(A&).
struct T513R7LB
{
    double v = 2;
    T513R7LB() noexcept {}
    T513R7LB(T513R7TA&) noexcept {}
};
inline T513R7LB t513R7MakeLB() noexcept { return T513R7LB(); }
struct T513R7XB
{
    double v = 2;
    T513R7XB() noexcept {}
    T513R7XB(const T513R7XB&) noexcept { t513R7Hits += 100; }
    T513R7XB(T513R7XB&&) noexcept { t513R7Hits += 1000; }
    ~T513R7XB() noexcept { t513R7Hits += 10000; }
};
inline T513R7XB t513R7SharedXB;
// An lvalue arm binds `operator XB&&() &`; an xvalue arm binds the `&&` overload. Neither copies.
struct T513R7XA
{
    int v = 1;
    ~T513R7XA() noexcept {}
    operator T513R7XB&&() & noexcept { t513R7Hits += 10; return static_cast<T513R7XB&&>(t513R7SharedXB); }
    operator T513R7XB&&() && noexcept { t513R7Hits += 20; return static_cast<T513R7XB&&>(t513R7SharedXB); }
};
inline T513R7XA t513R7SharedXA;
inline T513R7XA&& t513R7GetXA() noexcept { return static_cast<T513R7XA&&>(t513R7SharedXA); }
inline T513R7XB&& t513R7GetXB() noexcept { return static_cast<T513R7XB&&>(t513R7SharedXB); }
// T513 r8: a prvalue arm into a by-value B(A) parameter is elided (no A move); the parameter dies
// after the discarded B, at the end of the full expression. Digits record the order of events.
inline int t513R8Seq = 0;
inline void t513ResetR8() { t513R8Seq = 0; }
inline int t513R8Stats() noexcept { return t513R8Seq; }
struct T513R8A
{
    int v = 1;
    T513R8A() noexcept {}
    T513R8A(const T513R8A&) noexcept { t513R8Seq = t513R8Seq * 10 + 1; }
    T513R8A(T513R8A&&) noexcept { t513R8Seq = t513R8Seq * 10 + 2; }
    ~T513R8A() noexcept { t513R8Seq = t513R8Seq * 10 + 3; }
};
struct T513R8B
{
    double v = 2;
    T513R8B() noexcept {}
    T513R8B(T513R8A) noexcept { t513R8Seq = t513R8Seq * 10 + 4; }
    ~T513R8B() noexcept { t513R8Seq = t513R8Seq * 10 + 5; }
};
inline T513R8A t513R8MakeA(int v) noexcept { T513R8A a; a.v = v; return a; }
inline T513R8B t513R8MakeB() noexcept { return T513R8B(); }
// T513 r9: constructor-expression and nested-conditional prvalue arms reach B(A) without a move too.
struct T513R9A
{
    int v = 1;
    T513R9A(int x) noexcept : v(x) {}
    T513R9A(const T513R9A&) noexcept { t513R8Seq = t513R8Seq * 10 + 1; }
    T513R9A(T513R9A&&) noexcept { t513R8Seq = t513R8Seq * 10 + 2; }
    ~T513R9A() noexcept { t513R8Seq = t513R8Seq * 10 + 3; }
};
struct T513R9B
{
    double v = 2;
    T513R9B() noexcept {}
    T513R9B(T513R9A) noexcept { t513R8Seq = t513R8Seq * 10 + 4; }
    ~T513R9B() noexcept { t513R8Seq = t513R8Seq * 10 + 5; }
};
inline T513R9A t513R9MakeA(int v) noexcept { return T513R9A(v); }
inline T513R9B t513R9MakeB() noexcept { return T513R9B(); }
inline void t513R9Mark(int digit) noexcept { t513R8Seq = t513R8Seq * 10 + digit; }
// T513 r10: a producer that throws inside the conversion still unwinds the CFlat frame's cleanups.
inline int t513R10Live = 0;
inline int t513R10Dead = 0;
struct T513R10Guard
{
    T513R10Guard() noexcept { ++t513R10Live; }
    ~T513R10Guard() { --t513R10Live; ++t513R10Dead; }
};
inline void t513ResetR10() { t513R10Live = 0; t513R10Dead = 0; }
inline int t513R10Living() noexcept { return t513R10Live; }
inline int t513R10Deaths() noexcept { return t513R10Dead; }
inline T513R9A t513R10FailA(int x) { throw x; }
struct T513R10ThrowA
{
    int v;
    T513R10ThrowA(int x) { throw x; }
    ~T513R10ThrowA() {}
};
struct T513R10ThrowB
{
    double v = 2;
    T513R10ThrowB() noexcept {}
    T513R10ThrowB(T513R10ThrowA) noexcept {}
    ~T513R10ThrowB() {}
};
inline T513R10ThrowB t513R10MakeThrowB() noexcept { return T513R10ThrowB(); }
inline int t513R10Guarded(int (*f)(int), int x) { try { return f(x); } catch (int e) { return -e; } }

// T513 value sinks: digits record the A conversion, B construction, and destruction order.
inline int t513V10Sequence = 0;
inline void t513V10Reset() { t513V10Sequence = 0; }
inline int t513V10Stats() noexcept { return t513V10Sequence; }
struct T513V10A
{
    int value;
    T513V10A(int v) noexcept : value(v) { t513V10Sequence = t513V10Sequence * 10 + 1; }
    T513V10A(const T513V10A& other) noexcept : value(other.value) {
        t513V10Sequence = t513V10Sequence * 10 + 2;
    }
    T513V10A(T513V10A&& other) noexcept : value(other.value) {
        t513V10Sequence = t513V10Sequence * 10 + 3;
    }
    ~T513V10A() noexcept { t513V10Sequence = t513V10Sequence * 10 + 4; }
};
struct T513V10B
{
    int value;
    T513V10B(int v = 20) noexcept : value(v) {
        t513V10Sequence = t513V10Sequence * 10 + 5;
    }
    T513V10B(T513V10A a) noexcept : value(a.value + 100) {
        t513V10Sequence = t513V10Sequence * 10 + 6;
    }
    ~T513V10B() noexcept { t513V10Sequence = t513V10Sequence * 10 + 7; }
};
inline T513V10A t513V10MakeA(int v) noexcept { return T513V10A(v); }
inline T513V10B t513V10MakeB() noexcept { return T513V10B(20); }
// T513 r5: a view over the converted-from A; the sink must keep A alive through the call.
struct T513V11View
{
    const int* p;
    T513V11View() noexcept : p(nullptr) { t513V10Sequence = t513V10Sequence * 10 + 5; }
    T513V11View(const T513V10A& a) noexcept : p(&a.value) { t513V10Sequence = t513V10Sequence * 10 + 6; }
    T513V11View(const T513V11View& o) noexcept : p(o.p) { t513V10Sequence = t513V10Sequence * 10 + 8; }
    ~T513V11View() noexcept { t513V10Sequence = t513V10Sequence * 10 + 7; }
    int get() const noexcept { return p ? *p : 20; }
};
inline T513V11View t513V11MakeNull() noexcept { return T513V11View(); }
