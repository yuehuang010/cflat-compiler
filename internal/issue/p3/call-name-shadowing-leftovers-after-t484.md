# T504: bare-call name shadowing leftovers after T484

Closed in the T511 run: a local is visible to direct call lookup from its point of declaration,
including lambda bodies lexically nested in that local's initializer. A generic function body
instantiated during the initializer remains outside that lexical scope. Regression legs are in
`Test/test_basic.cb` and `Test/errors/err_noncallable_name_shadows_function.cb`.

Four related cases remain.

## Implicit-this generic member template call

Repro: `scratch/t504/generic_method_inferred_root_shadow.cb` and
`scratch/t504/generic_method_inferred_unique.cb`.

Observed: C++20 accepts both and returns 13 through `Holder::gv<int>`. The worktree still
reports unknown/undefined `gv`; the master executable failed the same way before edits.

Root cause: generic member templates are stored separately from ordinary function overloads, and
the bare-call path does not finish template inference and receiver binding before ordinary name
resolution. An attempted local dispatch reached the instantiated candidate but the call builder
received only the implicit receiver and no explicit argument, so it still reported no overload.

Fix direction: resolve the enclosing generic member template before outer-name fallback, then pass
the receiver and explicit arguments through the same successful member-call binding path.

## Qualified static method with int-to-double argument conversion

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
