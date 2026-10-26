# Bare-call name shadowing leftovers after T484

T484 (2026-10-24) refuses a bare call whose closest visible name is a non-callable variable. Left (master same):

## Self-initializer call

Summary: A local declarator can call a function with its own name in the initializer.
Repro: `scratch/t484r2/plain_id.cb` (`int plainId = plainId(4);`).
Observed: master and the T484 round-1 binary compile and run it with exit 5; clang++ -std=c++20 rejects it because the local int is the called object.
Root cause: local storage/name registration happens after the initializer expression is parsed, so lookup selects the outer function during the initializer.
Fix direction: align local-name visibility in initializers with the C++ point of declaration and diagnose this call while preserving valid initializer lookup.
Found: T484 round 2, 2026-10-24; pre-existing, intentionally not fixed in this run.

## Static member method vs local (T484 r2 review)
`struct S { static int gv(int); int call() { int gv = 2; return gv(3); } }` - master and T484 accept (exit 4), clang refuses;
the instance-method twin is refused by T484. `hasImplicitMemberFunction` (MainListener_PostfixExpression.cpp ~5532) uses
HasCFlatMemberMethod (~1583), which only matches methods whose first parameter is `this`; count static methods at the member level.

## Clang-accepted shapes refused on master and T484 (same error, not the T484 guard)
- implicit-this call to a generic method `gv<T>` while a root `gv` variable exists: "function 'gv' is not known".
- a method of a struct in namespace N calling `gv` where N has a `gv` variable: "function 'N.gv' is not known".
- `N.M.gv(double)` called with an int while N has a `gv` variable: "no overload matches".
