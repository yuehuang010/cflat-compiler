# C++ brace-argument overload ties clang calls ambiguous: leftovers after T314

CVAMB (2026-11-09) fixed: `pb2({1,2},{3,4})`, `pa(1,{1,2})`, `m.mq(1,{1,2})`, `da({1,2})` (default-argument
extra parameter), `fl({1})` (float vs long double), `m.mbb({1,2})` and `m.mvv({1,2})` (both & / both volatile)
now refuse; `m.mc({1,2})` (const member on non-const object), `m.mrr` (const& vs &), `m.mvq` (volatile vs
unqualified) and `pd({1,2},1)` now pick clang's overload. Cosmetic candidate-list spelling is still open.

Still open (clang picks the template, CFlat picks the non-template):
- template candidate `tp(il<T>)` vs `tp(il<long>)` with `{1,2}`: clang 2, CFlat 1. A function template with a
  braced list is not deduced by CFlat at all (template-only `tq({1,2})` reports "Undefined variable"), and beside a
  non-template the template route is not requested, so RequestCxxFreeBraceCall picks the non-template. Needs a
  braced-list template deduction model; not attempted.

Also open (wrong on master too, different pick): `f(const WV&,long)`, `f(WV,int)`, `f(W2,int)` called with a class
that has both `operator WV` and `operator W2`, `f(u,1)`: clang reports ambiguous, CFlat picks `f(WV,int)` (master
picked `f(const WV&,long)`).

## Fix direction

Cosmetic: the ambiguity candidate list spells `std::initializer_list<...>` and unqualified member names (`'m'`).
