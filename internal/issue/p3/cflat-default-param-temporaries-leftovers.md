# CFlat default-parameter temporaries: remaining caller lifetime

DEFTMP (2026-11-18) evaluates an omitted native default at the call site. This covers by-value
structs, alias, pointer, string and view parameters, written as an expression, a brace list or
`default`. Its temporaries now live to the end of the caller's statement, like an explicit
argument, and are destroyed once. The filed `fRd(mkN(1), N(5))` double destroy did not reproduce:
`return d` copies the borrowed parameter.

Wrapper residue. These defaults still go through the forwarding wrapper:
- a default that names an earlier parameter;
- any default when the caller sits above the callee (ForwardRefScanner marks only C++ class
  defaults);
- unique<T>, closure, function-pointer and C++-holder parameters.
- defaults whose value is itself a borrow: an alias- or pointer-returning call such as
  `= pick()` where `alias P pick(alias P d = mkP(6))`.

For these, native temps die when the wrapper returns, not at the end of the caller's full
expression. If the wrapper's return type can carry a borrow (pointer, alias, view, or a struct
holding one), the native temps are not destroyed at all, so the borrowed result stays valid:
- `int* pvE(P first, alias P d = mkP(first.v + 2))`, `*pvE(P(2))`: value 4, 1 object leaked;
- `alias P pickE(P first, alias P d = mkP(first.v + 1))`: value 3, 1 object leaked.
- `alias P pick2(alias P d = pick())`, `pick2().v`: value 6, 1 object leaked (master: same);
- `alias P gpk<T>(T x, alias P d = pick())`, `gpk(1).v`: value 6, 1 leaked (master: same);
- `int* byp2(int* p = pv())`, `*byp2()`: value 6, 1 leaked (master: same).
With a non-borrowing return (`int byv(P d = pick())`, `int bya(alias P d = pick())`) the
value is 6 and nothing leaks (master leaked 1).

With an int return, `plainE(P(2))` gives value 5 and nothing leaks.

Related: omitted defaults on C++ functions use CDEFARG caller-slot initialization. CFlat class
defaults that resolve an earlier formal parameter stay in the forwarding wrapper because they
cannot be evaluated independently at the call site, and retain the wrapper's early-destroy behavior.

Round 3 residue: C++ class-typed, pointer/nullptr, string-literal, and nested-brace elements in
C++ class parameter defaults are refused with a clean diagnostic. Three shared-helper attempts
preserved class storage but still trapped or produced an extra constructor, and did not match
clang for nullptr/string arguments. Scalar non-null non-string brace elements remain supported;
the refused shapes match master's prior clean-refusal behavior. Earlier-parameter brace defaults
remain accepted because scalar `int f(int a, int b = a)` is also accepted by CFlat.
