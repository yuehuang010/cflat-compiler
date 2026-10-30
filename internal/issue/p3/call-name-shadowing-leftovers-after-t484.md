# T504: bare-call name shadowing leftovers after T484

Closed in the T511 run: a local is visible to direct call lookup from its point of declaration,
including lambda bodies lexically nested in that local's initializer. A generic function body
instantiated during the initializer remains outside that lexical scope. Regression legs are in
`Test/test_basic.cb` and `Test/errors/err_noncallable_name_shadows_function.cb`.

Three related cases remain; all need a ruling.

## Qualified static method with int-to-double argument conversion

RULED 2026-10-29 (main session, from precedent; maintainer may veto): STANDS as refused (q08 680d9e8a + 2026-09-30 "C++ call args keep CFlat's strict conversions"). Working as
intended; this section is closed. The spelling is an explicit `(double)x` cast.

Repro: `scratch/t504/qualified_static_overload.cb` and
`scratch/t504/qualified_static_unique_int.cb`.

Observed: Clang accepts both with value 42 through `N::M::gv(double)`. CFlat finds that candidate
but rejects the int argument as a mismatch, with or without `N.gv`; the master executable failed
the same way before edits.

Fix direction: support the standard int-to-double conversion in this qualified overload path. The
namespace variable is not the cause: removing it leaves the same candidate and diagnostic.

## Field default calls its own field name

RULED 2026-10-29 (main session, from precedent; maintainer may veto; C++ member-initializer lookup; same ruling as cpp-class-field-default-construction-leftovers-after-v20
item 4): a field initializer sees the struct's members and members hide outer names, so `int gv = gv(1);` is the
non-callable-name error. An EARLIER field is readable (`int b = a + 1;`, declaration-order initialization); the field itself
or a LATER field in its own initializer is a located use-before-initialization error.

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

RULED 2026-10-29 (main session, from precedent; maintainer may veto): the local is in scope from its point of declaration for every lookup (T511 did calls). `int x = x;` is
a located error ("'x' is read in its own initializer"); the outer global is never the referent. The pinned leg
`t504ReadSelfInitializer` (value 9) becomes an err_ leg. Rust's outer-binding reading is NOT adopted: C/C++ scoping was
already chosen for calls.

Repro: `Test/test_basic.cb::t504ReadSelfInitializer`.

Observed: CFlat reads the global `t504SelfReadGlobal` and returns 9. In C++20 the local is in scope
in its own initializer, but reading it before initialization has undefined behavior, so there is no
stable value oracle.

Root cause: expression name lookup in a local initializer still resolves the outer global before
the newly declared local for non-call reads.

Fix direction: handle the local's point of declaration in ordinary expression lookup as well as
call lookup, with a defined diagnostic or initialization rule.

The generic-outer leftovers are all closed: NEST (1cc0ed5d2), NEST2 (1cc0ed5d2), NEST3 (1cc0ed5d2), NEST5 (ef83ffa02),
NEST6 (ef83ffa02) and NESTVAL (ef83ffa02, nested member bodies using the enclosing aggregate by value). Only the sections
above that need a ruling remain.
