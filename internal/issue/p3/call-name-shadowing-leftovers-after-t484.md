# T504: bare-call name shadowing leftovers after T484

Closed in the T511 run: a local is visible to direct call lookup from its point of declaration,
including lambda bodies lexically nested in that local's initializer. A generic function body
instantiated during the initializer remains outside that lexical scope. Regression legs are in
`Test/test_basic.cb` and `Test/errors/err_noncallable_name_shadows_function.cb`.

Three related cases remain (need a ruling), plus a generic-outer leftovers section at the end (no ruling needed).

## Qualified static method with int-to-double argument conversion

NEEDS RULING: CFlat call arguments never accept int -> double (q08 680d9e8a); accepting it is a call-conversion surface change, not a lookup fix.

Repro: `scratch/t504/qualified_static_overload.cb` and
`scratch/t504/qualified_static_unique_int.cb`.

Observed: Clang accepts both with value 42 through `N::M::gv(double)`. CFlat finds that candidate
but rejects the int argument as a mismatch, with or without `N.gv`; the master executable failed
the same way before edits.

Fix direction: support the standard int-to-double conversion in this qualified overload path. The
namespace variable is not the cause: removing it leaves the same candidate and diagnostic.

## Field default calls its own field name

Repro: `scratch/t511/field_default_self_call.cb`.

Observed: the master executable accepts `struct S { int gv = gv(1); };` and runs with value 2 by
calling the free `gv`. Clang rejects the expression because the field `gv` is an `int` and hides the
free function.

Root cause: field defaults see no members at all (even `int b = a + 1;` with a sibling field `a` fails with
"Undefined variable a." - C++ accepts it), so this is member-initializer lookup, not the T504 pending-local scope.
The method-body twin is already refused.

Fix direction: make the field name and type visible to call lookup in its own initializer, without
routing the field through the function-body pending-local scope.

## Non-call self-initializer read

Repro: `Test/test_basic.cb::t504ReadSelfInitializer`.

Observed: CFlat reads the global `t504SelfReadGlobal` and returns 9. In C++20 the local is in scope
in its own initializer, but reading it before initialization has undefined behavior, so there is no
stable value oracle.

Root cause: expression name lookup in a local initializer still resolves the outer global before
the newly declared local for non-call reads.

Fix direction: handle the local's point of declaration in ordinary expression lookup as well as
call lookup, with a defined diagnostic or initialization rule.

## Generic outer leftovers after NEST3 (no ruling needed)

NEST (2d2e922ad), NEST2 (24e3ec23a) and NEST3 (30e9629ad) cover bare calls from nested structs, field defaults and lambdas
to a generic outer's static members, defaulted generic methods, and inner-hides-outer under a generic. Still failing the
same way on master (clang++ -std=c++20 accepts each):

- A member template inside a generic that uses the outer `U`: "cannot find the type 'U'".
- Calling a sibling nested type's static (`I.go()` from a method of sibling `M`) inside a generic: "Undefined variable".
- Qualified call through a namespace to a generic's static (`N.O<long>.run()`): "Undefined variable".
- A defaulted INTERFACE method called through an interface variable with the argument omitted (`IH p = c; p.h(1);`):
  "no version of 'IH.h' takes 1 argument(s)" (clang uses the base default, 13). NEST4 (6923945d8) fixed the bare implicit-this form.
- A bare call to a defaulted instance method from a STATIC method is refused (correct) but with "no overload ... [0] i8 <this>"
  instead of the "non-static member function without an object argument" text the non-defaulted form gets.
- Using the outer aggregate by value from a nested body (`O<long>` / `O` incomplete): layout ordering, generic and not.
- The generic redefinition diagnostic prints `G<long.h>` instead of the qualified method name.
