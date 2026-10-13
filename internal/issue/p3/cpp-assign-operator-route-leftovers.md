# C++ assignment operator route leftovers

## Summary

Known assignment-routing gaps left outside C4+B6 round 3. These are active follow-ups, not claims that the current branch regressed them.

## Repros

- Namespace C++ global release is a no-op: `scratch/rev2/d.cb`, `_ = move gv.nsg;` and `_ = move gv.nsgn;` emit no destructor and leave the live flag untouched. The round-2 review reproduced this on master too.
- Templated `operator=(T)` is not selected for a scalar source; C++ accepts it.
- `operator=(std::nullptr_t)` is not selected for `nullptr`.
- `using Base::operator=` does not expose the base overload on a derived type.
- `operator=(int)` plus `operator=(double)` with `1U` is rejected/picked differently; clang reports ambiguity.
- Chained `lx = (ly = 5)` where `GX operator=(const GX&)` returns by value: the outer operator's by-value result is never destroyed (copy=1 dtor=1, clang dtor=2). Pre-existing on master for locals; the global inner form has the same count.
- Selector texts (review P3-A): `a = 1L` on `operator=(int)` + `operator=(double)` says "resolves to operator=(int) ... ('int' to 'int')" where clang reports ambiguity for a long source; deleted `operator=(int)` + `operator=(double)` with `a = 1` names `operator=(double)` ('char' to 'double') where clang selects the deleted `operator=(int)`. Rejection is right, text is wrong: the literal is typed as its narrowest kind and deleted candidates are dropped before ranking.

## Root cause

The CFlat direct assignment selector does not model all C++ overload candidate classes or deleted/ambiguous ranking. Global C++ namespace release does not flow through the ordinary global storage/live-flag ownership route. Chained outer by-value assignment results are not destroyed.

## Fix direction

Handle namespace global move/release through the same global live-state and destructor lowering used by supported globals. Extend assignment selection for templates, inherited `using` overloads, nullptr, implicit conversion fallback, and deleted/ambiguous candidates without introducing per-statement clang work. For value expressions, preserve the selected return category: support the actual reference result or issue a diagnostic when the result cannot be represented; reject void results.

## T102 status (2026-10-10)

- Fixed in T102: scalar assignment to a trivial class with a converting constructor and implicit copy assignment. The new non-template converting-constructor fallback is limited to constructor-bearing classes without a user-declared assignment operator or assignment template; the ordinary same-class trivial-copy path stays in place. Leg 3289 checks value and exactly one converting temporary (two constructor calls total).
- The first integration retry exposed an existing regression at leg 7052 (`XvTrivialAssign`): the constructor fallback stole a templated assignment. It is now excluded by `hasAssignTemplate`; the instrumented scratch copy identified the leg, and the full interop test passed afterward.
- Open: inherited `using Base::operator=` remains rejected for `a = 4`; earlier targeted attempts exhausted the three-attempt budget.
- Open: simple standalone scalar `operator=(T)` and `operator=(std::nullptr_t)` probes passed on master and clang; exact reported shapes remain unverified. Unsigned/deleted candidate ranking and diagnostic text, chained by-value result destruction, and namespace-global release remain unresolved.
