# Mixed-class C++ conditional residue after MIXRES

MIXCOND (af5834ad3), MIXCOND2 (483f3b83d), MIXCOND3 (8844053c8) and MIXRES (2026-11-19) landed the mixed A/B
conditional (B has a converting ctor from A) at declarations, returns, free-function / method / CFlat call
arguments, by-value parameter lvalue arms (S3), lvalue arms through a by-value converting ctor at C++ member
callees, and a mixed `?:` nested in a mixed `?:` call argument (by-value and `const T&` parameters).
clang++ -std=c++20 oracle; legs in Test/test_cpp_interop.cb (T513Mixres*, codes 95300-95316).

Header for every repro below (Test/library/cpp_interop_mixcond3.h, trace digits: A 1 ctor / 2 copy / 3 move /
4 dtor, BV 5 ctor(int) / 6 ctor(A) / 8 copy / 9 move / 7 dtor):

```cpp
namespace t513mix {
inline int t513M3Sequence = 0;
struct A { int v; A(int n) noexcept; A(const A&) noexcept; A(A&&) noexcept; ~A() noexcept; };
struct BV { int v; BV(int n) noexcept; BV(A a) noexcept /* v = a.v + 100 */; BV(const BV&) noexcept;
            BV(BV&&) noexcept; ~BV() noexcept; };
inline A mkA(int value) noexcept { return A(value); }
inline BV mkBV(int value = 20) noexcept { return BV(value); }
inline int ctakeBV(BV value) noexcept { return value.v; }
struct W { int v; W(BV b) noexcept : v(b.v + 1000) {} };   // repro-only, not in the test header
}
```

## Still refused (clang accepts)
- C++ CONSTRUCTOR call argument: any mixed conditional, prvalue arms included, is refused ("ternary branches
  have incompatible types 't513mix.BV' and 't513mix.A'"), master and MIXRES alike:
  `t513mix.W w = t513mix.W(k ? t513mix.mkBV(9) : t513mix.mkA(3));` (clang 1103, trace 1674),
  `t513mix.W(k ? a : t513mix.mkBV(20))` (clang 1103 / 126744), nested `t513mix.W(k ? (j ? mkA(1) : mkBV(2)) : mkA(3))`.
  Root cause (unverified): the ctor-call argument path does not enter the call-argument mixed join
  (`callArgument` in lowerMixedCxxValueJoin) - no expected parameter type reaches the `?:`.
- Nested mixed `?:` into a GENERIC CFlat callee is refused ("ternary branches have incompatible types
  't513mix.BV' and 't513mix.A'"); the flat form `gg(k ? mkBV(1) : mkA(2))` works (= clang 1/57, 102/1674).
  clang (`template<class T> int gg(T v)`) gives 1 102 3 3 for (k,j) = 11,10,01,00:
  `int gg<T>(T v) { return v.v; }` / `gg(k ? (j ? t513mix.mkBV(1) : t513mix.mkA(2)) : t513mix.mkBV(3))`.
  Cause: the nested slot claim needs the parameter class before the arms parse; a generic parameter has none
  until deduction from the join.
- A header reached through a wrapper `#include` (umbrella header) refuses even the flat lvalue by-value arm
  `ctakeBV(k ? a : mkBV(3))` (clang 107 107 3 3); the direct import works (pre-existing).
- Nested mixed `?:` into a `std::string` (or other class) parameter whose leaves are non-class values beside
  class leaves stays refused (legacy, pre-existing).

## Count / order (values match clang, no leak)
- Moved lvalue arm `k ? move a : mkBV(20)` with `BV(A a)`: CFlat relocates `a` into the by-value `A` parameter
  (no move ctor, one A dtor), clang move-constructs it. Call argument `ctakeBV(k ? move a : mkBV(20))`:
  CFlat 1674 vs clang 136744 - same as the plain `ctakeBV(move a)` (1674 vs 136744), so not conditional-specific.
  Declaration `BV r = k ? move a : mkBV(20);` CFlat 1674 vs clang 136474 and return
  `BV f(bool k, A a) { return k ? move a : mkBV(20); }` CFlat 1647 vs clang 136447 ARE conditional-specific:
  the plain `BV r = move a;` matches clang (136474).
- CFlat struct temps with a user dtor are not interleaved with C++ temps in reverse construction order:
  `take(mkA(useS(mkS(3))))` gives `ac[syx` vs clang `ac[yxs` (plain calls too; repro: the shape in this line with
  a CFlat `struct S { ~S(){...} }` temp and a C++ temp in one call). Direction: one shared ledger order for CFlat
  and C++ temps.
- A temp in the right operand of `&&` dies at the `&&`, not at the end of the full expression (`1a[xac[yx` vs
  clang `1a[ac[yxx`).
- Lambda arm: one extra move vs clang (pre-existing).
- Residual risk (no probe reached it): hoistArmStructTemps advances the flush mark past a temp
  HoistOwnedStructTempTo fails to hoist (non-entry alloca / dead insert block) - such a temp would leak.

## Cross target
- `--platform win64` cross-compile on macOS refuses EVERY mixed A/B conditional call argument ("no common C++
  type"), defaults or not. Host-native Windows builds accept them, so the cross target loses the converting-ctor
  lookup. Repro: `int r = t513mix.ctakeBV(k ? t513mix.mkA(11) : t513mix.mkBV(20));` compiled with
  `--platform win64 -o x.exe` on macOS.
- MIXRES legs check exact order on Itanium only; on Windows they check value and ctor/dtor balance (the MS ABI
  destroys by-value parameters in the callee, so the order differs by design).
