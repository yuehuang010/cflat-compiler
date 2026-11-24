# C++ call parity leftovers (pre-existing; residue after CALLPAR and CALLRES, fix box 16)

Measured 2026-11-19 against clang++ -std=c++20. Counts are ctor/copy/move/dtor/live digits of cpptw::Twin
(Test/library/cpp_interop_twin.h), read after the scope closes. CALLRES fixed: ordinary lookup plus ADL is one
candidate set - the visible CFlat overloads go into the clang ADL request as deleted stand-ins, so a better C++
candidate binds, a tie is refused as ambiguous and a stand-in's win keeps the CFlat lookup (namespace functions,
function templates and hidden friends; a CFlat generic stands in with its own `template <class T0> f(T0)` shape;
legs CallresTake..CallresGenericShape, Test/errors/err_cpp_adl_visible_ambiguous.cb).

Header for the repros below (import it directly; see item 3):

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
struct RS { int v = 0; };   // CFlat side of item 4: `struct RS { int v = 0; };`
namespace rv   // item 4
{
    struct W { int v; W(int x) : v(x) {} };
    inline int ty(W, int) { return 2; }
    template <class T> int g1(T) { return 2; }
}
```

1. Member set holding a non-template and a member template, `?:` argument: one extra move per arm. The set
   always routes through the clang wrapper (deduceBesideSpecializations), which receives the joined slot as
   `T&&` and moves it on. Deferring the `?:` into the wrapper (as template-only sets now do) reaches the
   non-template by-value binder instead and refuses ("cannot pass C++ class ... by value").
   `cp.Host h = default; cpptw.Twin x = cpptw.Twin(7); h.mix(false ? default : x)`: 17 11130, clang 17 11020;
   `h.mix(true ? default : x)`: 10 20130, clang 10 20020. (T418TemplateSingle/Competing legs are this shape.)

2. Ctor `S(A&... rest, int a, int b)` (non-final reference pack) reads the by-value int late:
   `int x = 0; cp.pack_lead_value(cp.PackLead(x, cp.bump(&x)))` gives 1110, clang 1010 (T416 keeps the
   conservative slot rule; see fix-issue-lessons digest T416).

3. ADL only knows names from a DIRECTLY imported header: when cpp_interop_lastuse.h is reached through
   another header's `#include` (`import cpp "h.h";` with `h.h` including it), a bare
   `callpar.adl.A a = default; adlf(a)` is "Undefined variable adlf." and a bare `take(x)` beside a CFlat
   `int take(int z)` keeps the CFlat pick (1 10010); clang 6 / 7 11020. IsCxxNamespaceFunctionName and the
   associated-namespace check read only registered names.

4. A visible CFlat overload whose parameter has no C++ spelling (a CFlat struct) disables the joint ranking for
   the whole set, so the CFlat-only pick stays: `int take(RS s) { return 3; }` and `int take(int z) { return z; }`
   beside cpptw::take, `take(x)` gives 1 10010, clang 7 11020. A stand-in for such a parameter needs to know
   whether a native converting constructor takes the argument.
   The same skip applies to a CFlat enum parameter or argument (it would spell as its backing integer and
   tie with an `int` overload): `int ty(W w, long c)` beside `rv::ty(W, int)` called with a CFlat enum keeps the
   CFlat pick, where clang promotes to int. A CFlat generic whose parameters are not bare type parameters
   (`f<T>(T[] v)`, `f<T>(list<T> l)`) or has a defaulted parameter (`gd<T>(T a, T* b = nullptr)` beside
   `template <class T> int rv::gd(const T&)`: CFlat pick 1, clang ambiguous) and a generic not yet instantiated for the call (`g1<T>(T* a)` called
   with a value beside `template <class T> int rv::g1(T)`: refused "no overload", clang 2) are skipped too.

5. ADL through a conversion operator ends the converted class temporary before the caller's full-expression.
   With `T::live` incremented by construction and decremented by destruction, `U::operator T()` returning
   `T(5100)`, `int cv(T value) { return value.value; }`, and
   `int observe(int value) { return value * 10 + T::live; }`, both
   `observe(cv(u))` for an lvalue `U u(1)` and `observe(cv(U(1)))` produce `51000` in CFlat and `51001`
   with clang++ -std=c++20. Copies and moves are zero; `T::live` is zero after the full-expression in both.
   The repro was measured as packed values `51000000` vs `51001000` for each source category.
   Root cause: `EmitCxxImplicitArgumentConversions` registers a `__cflat_udc_*` wrapper taking `U&` or
   `U&&` and returning `cv(p0)`. The implicit conversion to by-value `T` happens in that wrapper, so its
   temporary ends before a following call in the caller's full-expression. Fix direction: preserve the
   selected `cv(T)` candidate but materialize its conversion result in caller-owned full-expression storage,
   then call the native by-value target with matching cleanup and unwind handling.

6. Two by-value prvalue shapes still move once more than clang (ADLMIX review 1, master same). Header:
   `struct H { int v; H(int x) : v(x) {} H(const H& o) : v(o.v + 100) {} H(H&& o) : v(o.v + 10) {} };` (no dtor).
   - Defaulted class parameter: `inline int def(H a, H b = H(1)) { return a.v * 10000 + b.v; }`, `def(H(5))` gives
     150010, clang 50010 (the explicit prvalue takes one extra move when a default fills a later parameter).
   - `?:` argument with prvalue arms: `inline int one(H a) { return a.v; }`, `one(t ? H(3) : H(4))` gives 13, clang 3;
     `mix(t ? H(3) : lv, H(6))` gives 13006, clang 3006 (the joined prvalue is materialized then moved; see item 1).
