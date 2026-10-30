#pragma once

#include "cpp_interop_twin.h"

namespace cpptw
{
    inline int take(Twin value) { return value.value(); }
    inline int take2(Twin first, Twin second) { return first.value() + second.value(); }
    inline int t = 30;
    inline int nested_global() { return t; }
    inline int packed_counts() { return copy_count() * 100 + move_count() * 10 + dtor_count(); }
}

// T418: `c ? default : local` arguments where the local's class differs from the parameter's,
// and reference parameters whose `?:` is a prvalue in C++ (a copy, or the T&& overload).
namespace t418
{
    inline int* counts() { static int c[5] = { 0, 0, 0, 0, 0 }; return c; }
    inline void reset() { for (int i = 0; i < 5; ++i) counts()[i] = 0; }
    // ctor, copy, move, dtor, live allocations, packed one decimal digit each.
    inline int stats()
    {
        int* c = counts();
        return c[0] * 10000 + c[1] * 1000 + c[2] * 100 + c[3] * 10 + c[4];
    }
    struct Val
    {
        int* p;
        Val() : p(new int(0)) { ++counts()[0]; ++counts()[4]; }
        explicit Val(int n) : p(new int(n)) { ++counts()[0]; ++counts()[4]; }
        Val(const Val& o) : p(new int(*o.p)) { ++counts()[1]; ++counts()[4]; }
        Val(Val&& o) noexcept : p(o.p) { o.p = nullptr; ++counts()[2]; }
        ~Val() { if (p != nullptr) --counts()[4]; delete p; ++counts()[3]; }
        int value() const { return p != nullptr ? *p : -1; }
    };
    // Converts to Val through a user conversion operator (a different class arm).
    struct Conv
    {
        int field;
        Conv() : field(4) {}
        Conv(const Conv& o) : field(o.field) {}
        ~Conv() {}
        operator Val() const { return Val(field); }
    };
    inline int take_conv(Val v) { return v.value(); }
    inline int tref(const Val& v) { return 20 + v.value(); }
    // Copy from a const lvalue must pick CV(const CV&) (+200), never CV(CV&) (+100).
    struct CV
    {
        int n;
        CV() : n(0) {}
        explicit CV(int a) : n(a) {}
        CV(CV& a) : n(a.n + 100) {}
        CV(const CV& a) : n(a.n + 200) {}
        ~CV() {}
    };
    struct Host
    {
        int member(const Val& v) { return 30 + v.value(); }
        int rr(Val&& v) { return 100 + v.value(); }
        int rr(const Val& v) { return 200 + v.value(); }
        int later(Val v, int b) { return v.value() * 100 + b; }
        int same(Val a, Val b) { return a.value() * 100 + b.value(); }
        // The middle slot's overloads disagree, so no `?:` argument of a call defers.
        int take3(Val a, Val b, Val c) { return a.value() * 100 + b.value() * 10 + c.value(); }
        int take3(Val a, int b, Val c) { return 900 + b; }
        // A non-template beside a member template: clang picks the non-template.
        int pick(Val a) { return 10 + a.value(); }
        template <class T> int pick(T a) { return 20 + a.value(); }
        int pick2(Val a, Val b) { return 10 + a.value() * 10 + b.value(); }
        template <class T> int pick2(T a, int b) { return 20 + a.value(); }
        int cvmember(CV a) { return a.n; }
    };
    inline int mutate(Val& v) { *v.p = 9; return 2; }
    // A condition whose operator bool writes an earlier argument's local.
    inline Val* aimed = nullptr;
    inline void aim(Val& v) { aimed = &v; }
    struct Flag
    {
        Flag() {}
        Flag(const Flag&) {}
        ~Flag() {}
        operator bool() const { *aimed->p = 9; return false; }
    };
}

namespace t420field
{
    struct Wrap { cpptw::Twin tw; };
    namespace
    {
        inline int take(cpptw::Twin value) { return value.value(); }
    }
}

namespace t420move
{
    namespace
    {
        inline int take(cpptw::Twin value) { return value.value(); }
    }
}

namespace t420look
{
    namespace
    {
        inline int take(cpptw::Twin value) { return value.value(); }
    }
    inline int take(cpptw::Twin value) { return value.value(); }
    struct Look
    {
        int field;
        Look() : field(4) {}
        Look(const Look& other) : field(other.field) {}
        ~Look() {}
        operator cpptw::Twin() const { return cpptw::Twin(field); }
    };
}

namespace t420r2self
{
    struct L2
    {
        int field;
        L2() : field(4) {}
        L2(const L2& other) : field(other.field) {}
        ~L2() {}
        operator cpptw::Twin() const { return cpptw::Twin(field); }
    };
    namespace
    {
        inline int take(L2 value) { return 100 + value.field; }
    }
}

namespace t420r2overload
{
    struct L2
    {
        int field;
        L2() : field(4) {}
        L2(const L2& other) : field(other.field) {}
        ~L2() {}
        operator cpptw::Twin() const { return cpptw::Twin(field); }
    };
    namespace
    {
        inline int take(L2 value) { return 100 + value.field; }
        inline int take(cpptw::Twin value) { return 200 + value.value(); }
    }
}

namespace t461cv
{
    struct CV
    {
        int value;
        explicit CV(int n) : value(n) {}
        CV(CV& other) : value(other.value + 100) {}
        CV(const CV& other) : value(other.value + 200) {}
        ~CV() {}
    };
    inline int take(CV value) { return value.value; }
    template<class T> inline int template_take(T value) { return value.value; }
    struct Host { int member_take(CV value) { return value.value; } };
    struct Sink
    {
        int result;
        Sink(CV value) : result(value.value) {}
    };
    struct Box
    {
        CV item;
        explicit Box(int n) : item(n) {}
    };
    inline const CV& const_ref(const CV& value) { return value; }

    struct OnlyConst
    {
        int value;
        explicit OnlyConst(int n) : value(n) {}
        OnlyConst(const OnlyConst& other) : value(other.value + 200) {}
        ~OnlyConst() {}
    };
    inline int take_only_const(OnlyConst value) { return value.value; }
    struct OnlyConstHost { int member_take(OnlyConst value) { return value.value; } };
    struct OnlyConstSink
    {
        int result;
        OnlyConstSink(OnlyConst value) : result(value.value) {}
    };

    struct OnlyNonConst
    {
        int value;
        explicit OnlyNonConst(int n) : value(n) {}
        OnlyNonConst(OnlyNonConst& other) : value(other.value + 100) {}
        ~OnlyNonConst() {}
    };
    inline int take_only_nonconst(OnlyNonConst value) { return value.value; }
    struct OnlyNonConstHost { int member_take(OnlyNonConst value) { return value.value; } };
    struct OnlyNonConstSink
    {
        int result;
        OnlyNonConstSink(OnlyNonConst value) : result(value.value) {}
    };
}

namespace t461r2
{
    struct TemplateCtor
    {
        int value;
        explicit TemplateCtor(int n) : value(n) {}
        TemplateCtor(TemplateCtor& other) : value(other.value + 100) {}
        template<class U> TemplateCtor(const U& other) : value(other.value + 50) {}
        ~TemplateCtor() {}
    };
    inline int take_template(TemplateCtor value) { return value.value; }
}

// T518: anonymous-namespace functions are not bound up front, so calls go through generated
// wrappers whose parameter types are unknown before the first call.
namespace t518
{
    namespace
    {
        inline int take(cpptw::Twin value) { return value.value(); }
        inline int take2(cpptw::Twin first, cpptw::Twin second) { return first.value() + second.value(); }
        inline int later(t418::Val v, int b) { return v.value() * 100 + b; }
        inline int lref(const t418::Val& v, int b) { return v.value() * 100 + b; }
        // Review families: overloads, templates and default arguments decide by the SELECTED callee.
        inline int mixed(t418::Val x, double n) { return 1000 + x.value() * 100 + (int)n; }
        inline int mixed(const t418::Val& x, int n) { return 2000 + x.value() * 100 + n; }
        inline int over(t418::Val x, int n) { return 1000 + x.value() * 100 + n; }
        inline int over(t418::Val x, double n) { return 2000 + x.value() * 100 + (int)n; }
        template <class T> int templ(T x, int n) { return 1000 + x.value() * 100 + n; }
        inline int defargs(t418::Val x, int n, int z = 0) { return 1000 + x.value() * 100 + n + z; }
        template <class T> int guardtmpl(t418::Val x, T n) { return 1000 + x.value() * 100 + n; }
        inline int guarddef(t418::Val x, int n = 2) { return 1000 + x.value() * 100 + n; }
        // A default argument that is not a constant: the by-address call cannot supply it.
        inline int dyndef(t418::Val x, int n, int z = t418::stats()) { return 1000 + x.value() * 100 + n + z; }
    }
    // A trivially copyable class is snapshot at its argument; a non-trivial one into a by-value
    // parameter is copy-constructed there and the callee called by address; references need no copy.
    struct Pod { int n; };
    inline int mutpod(Pod& p) { p.n = 9; return 2; }
    struct NC { int n; explicit NC(int a) : n(a) {} NC(const NC&) = delete; ~NC() {} };
    inline int mutnc(NC& x) { x.n = 9; return 2; }
    struct Inc
    {
        int n;
        explicit Inc(int a) : n(a) {}
        Inc(const Inc& x) : n(x.n + 1) {}
        ~Inc() {}
        int get() const { return 2; }
        int bump() { n = 9; return 2; }
    };
    inline int readinc(const Inc&) { return 2; }
    inline int incptr(Inc* x) { x->n = 9; return 2; }
    inline int inccast(const Inc& x) { const_cast<Inc&>(x).n = 9; return 2; }
    struct Mu
    {
        mutable int n;
        explicit Mu(int a) : n(a) {}
        Mu(const Mu& x) : n(x.n) {}
        ~Mu() {}
        int poke() const { n = 9; return 2; }
    };
    struct Arm { int n; Arm() : n(4) {} explicit Arm(int a) : n(a) {} Arm(const Arm& x) : n(x.n) {} ~Arm() {} };
    struct Target
    {
        int n;
        Target() : n(10) {}
        template <class T> Target(const T&) : n(20) {}
        Target(const Target& x) : n(x.n) {}
        ~Target() {}
    };
    namespace
    {
        inline int pod(Pod x, int n) { return 1000 + x.n * 100 + n; }
        inline int pod(Pod x, double n) { return 2000 + x.n * 100 + (int)n; }
        template <class T> int podtmpl(T x, int n) { return 1000 + x.n * 100 + n; }
        inline int poddef(Pod x, int n, int z = 0) { return 1000 + x.n * 100 + n + z; }
        inline int podref(const Pod& x, int n) { return 2000 + x.n * 100 + n; }
        inline int ncref(const NC& x, int b) { return 2000 + x.n * 100 + b; }
        inline int inctake(Inc x, int b) { return 1000 + x.n * 100 + b; }
        inline int incpeek(const Inc& x) { return x.n - 5; }
        inline int incbump(Inc& x) { x.n = 9; return 2; }
        inline int convert(Target x, int b) { return 1000 + x.n * 100 + b; }
        inline int mutake(Mu x, int b) { return 1000 + x.n * 100 + b; }
        // The by-address call names clang's selected specialization, explicit arguments included.
        inline int pick(Inc x, int b) { return 1000 + x.n * 100 + b; }
        template <class T> int pick(T x, int b) { return 2000 + x.n * 100 + b; }
        template <class T> int unusedt(Inc x, int b) { return 3000 + x.n * 100 + b; }
        // Specialization arguments naming anonymous-namespace entities (spelled source-valid).
        struct Tag { static constexpr int v = 3; };
        inline int tagTarget = 4;
        template <class T = Tag> int tagged(Inc x, int b) { return 2000 + T::v + x.n * 100 + b; }
        template <int* P = &tagTarget> int tagptr(Inc x, int b) { return 3000 + *P + x.n * 100 + b; }
    }
    // A callee-destroyed parameter passed in registers ([[clang::trivial_abi]], 8 and 16 bytes).
    inline int taCopies = 0, taDeaths = 0;
    inline void tareset() { taCopies = 0; taDeaths = 0; }
    inline int tastate() { return taCopies * 10 + taDeaths; }
    struct [[clang::trivial_abi]] Ta8
    {
        long long n;
        Ta8() : n(7) {}
        Ta8(const Ta8& x) : n(x.n) { ++taCopies; }
        ~Ta8() { ++taDeaths; }
    };
    struct [[clang::trivial_abi]] Ta16
    {
        long long n, m;
        Ta16() : n(7), m(0) {}
        Ta16(const Ta16& x) : n(x.n), m(x.m) { ++taCopies; }
        ~Ta16() { ++taDeaths; }
    };
    inline int tamut8(Ta8& x) { x.n = 9; return 2; }
    inline int tamut16(Ta16& x) { x.n = 9; return 2; }
    namespace
    {
        inline int tatake8(Ta8 x, int b) { return 1000 + (int)x.n * 100 + b; }
        inline int tatake16(Ta16 x, int b) { return 1000 + (int)x.n * 100 + b; }
    }
}
