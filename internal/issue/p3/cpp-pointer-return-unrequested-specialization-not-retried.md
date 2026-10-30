# P3: C++ compound-operator result leftovers after OPFREE

OPFREE (f13122adf) fixed the global free operator returning a class-template specialization, base-declared free operators on a
derived receiver, and different-type reference results of compound assignment (alias of the returned object). Finding 4 (copy-init
through a converting constructor) did not reproduce; master already matched clang. Queue ruling 2026-10-24 stands: no
`a = a @ b` fallback on C++ classes. Probes and oracles: scratch/repro_keep/opfree/. clang++ -std=c++20 is the oracle.

## Remaining (pre-existing; master behaves the same)

- A SAME-type reference result is a copy, not an alias: member `M& operator*=(int)` or free `F& operator+=(F&, int)` with
  `(a *= 3).bump()` mutates a copy (CFlat 1, clang 0); chained `(a *= 2) *= 3` and an operator returning a reference to a
  DIFFERENT object of the same type (`(a += 3).get()` reads `a`) behave the same. Cause: the alias branch in
  MainListener_Expressions.cpp (~8610) only fires when the result type differs from the destination type; same-type results
  re-read the destination.
- A global MEMBER compound operator returning a specialization reference (`GB<int>& GB<int>::operator+=(int)`) is still refused
  by the member-signature eligibility gate.
- The ambiguity diagnostic for global free operators prints the internal name `__cxx_free.operator*`.
