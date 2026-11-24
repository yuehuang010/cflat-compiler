# C++ call parity leftovers (pre-existing; residue after CALLPAR and CALLRES, fix box 16)

Measured 2026-11-19 against clang++ -std=c++20. Counts are ctor/copy/move/dtor/live digits of cpptw::Twin
(Test/library/cpp_interop_twin.h), read after the scope closes. CALLRES fixed: ordinary lookup plus ADL is one
candidate set - the visible CFlat overloads go into the clang ADL request as deleted stand-ins, so a better C++
candidate binds, a tie is refused as ambiguous and a stand-in's win keeps the CFlat lookup (namespace functions,
function templates and hidden friends; a CFlat generic stands in with its own `template <class T0> f(T0)` shape;
legs CallresTake..CallresGenericShape, Test/errors/err_cpp_adl_visible_ambiguous.cb).

Header for the repros below (import it directly; see item 4):

```cpp
#include "cpp_interop_lastuse.h"
namespace cp
{
    struct Host
    {
        int mix(cpptw::Twin a) { return 10 + a.value(); }
        template <class T> int mix(T a) { return 20 + a.value(); }
    };
    struct PackLead
    {
        int v;
        template <class... A> PackLead(A&... rest, int a, int b) : v(1000 + a * 100 + b * 10 + (int)sizeof...(A)) {}
    };
    inline int pack_lead_value(PackLead s) { return s.v; }
    inline int bump(int* x) { return ++*x; }
}
struct RS { int v = 0; };   // CFlat side of item 5: `struct RS { int v = 0; };`
namespace rv   // items 5 and 6
{
    struct W { int v; W(int x) : v(x) {} };
    inline int ty(W, int) { return 2; }
    template <class T> int g1(T) { return 2; }
    struct C { static int live; int v; C(int x) : v(x) { ++live; } C(const C& o) : v(o.v) { ++live; }
               C(C&& o) : v(o.v) { ++live; } ~C() { --live; } };
    inline int C::live = 0;
    inline int rr(C c) { return c.v; }
    inline int liveC() { return C::live; }
}
```

1. Member set holding a non-template and a member template, `?:` argument: one extra move per arm. The set
   always routes through the clang wrapper (deduceBesideSpecializations), which receives the joined slot as
   `T&&` and moves it on. Deferring the `?:` into the wrapper (as template-only sets now do) reaches the
   non-template by-value binder instead and refuses ("cannot pass C++ class ... by value").
   `cp.Host h = default; cpptw.Twin x = cpptw.Twin(7); h.mix(false ? default : x)`: 17 11130, clang 17 11020;
   `h.mix(true ? default : x)`: 10 20130, clang 10 20020. (T418TemplateSingle/Competing legs are this shape.)

2. An rvalue class argument through the ADL wrapper moves once more than clang: `take(cpptw.Twin(7))` (bare,
   reaching cpptw::take by ADL, with or without a visible CFlat `take(int)`) gives 7 10120, clang 7 10010. The
   wrapper's `call(Twin p0)` forwards `static_cast<decltype(p0)&&>(p0)` into the by-value parameter. The
   qualified `cpptw.take(rettw())` / `cpptw.take(default)` now match clang (10010), so the 2026-10-22
   free-wrapper numbers (1010020 vs 1000010) no longer reproduce; only the ADL form keeps the move.

3. Ctor `S(A&... rest, int a, int b)` (non-final reference pack) reads the by-value int late:
   `int x = 0; cp.pack_lead_value(cp.PackLead(x, cp.bump(&x)))` gives 1110, clang 1010 (T416 keeps the
   conservative slot rule; see fix-issue-lessons digest T416).

4. ADL only knows names from a DIRECTLY imported header: when cpp_interop_lastuse.h is reached through
   another header's `#include` (`import cpp "h.h";` with `h.h` including it), a bare
   `callpar.adl.A a = default; adlf(a)` is "Undefined variable adlf." and a bare `take(x)` beside a CFlat
   `int take(int z)` keeps the CFlat pick (1 10010); clang 6 / 7 11020. IsCxxNamespaceFunctionName and the
   associated-namespace check read only registered names.

5. A visible CFlat overload whose parameter has no C++ spelling (a CFlat struct) disables the joint ranking for
   the whole set, so the CFlat-only pick stays: `int take(RS s) { return 3; }` and `int take(int z) { return z; }`
   beside cpptw::take, `take(x)` gives 1 10010, clang 7 11020. A stand-in for such a parameter needs to know
   whether a native converting constructor takes the argument.
   The same skip applies to a CFlat enum parameter or argument (it would spell as its backing integer and
   tie with an `int` overload): `int ty(W w, long c)` beside `rv::ty(W, int)` called with a CFlat enum keeps the
   CFlat pick, where clang promotes to int. A CFlat generic whose parameters are not bare type parameters
   (`f<T>(T[] v)`, `f<T>(list<T> l)`) or has a defaulted parameter (`gd<T>(T a, T* b = nullptr)` beside
   `template <class T> int rv::gd(const T&)`: CFlat pick 1, clang ambiguous) and a generic not yet instantiated for the call (`g1<T>(T* a)` called
   with a value beside `template <class T> int rv::g1(T)`: refused "no overload", clang 2) are skipped too.

6. On the ADL wrapper path, the parameter copy of an lvalue by-value argument is destroyed before the enclosing
   full-expression ends: with `struct C { static int live; ... }` counting live objects and
   `inline int rr(C c) { return c.v; }`, the bare `rr(cc) * 10 + rv.liveC()` (no CFlat `rr`) gives 41 on master
   and branch, clang 42. The qualified `rv.rr(cc) * 10 + rv.liveC()` gives 42.
