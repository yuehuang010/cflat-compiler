# q02 - Generics and name resolution: instantiation timing and attribution

Four items. Three are symptoms of generic instantiations being NAMED / QUEUED at scan time,
before the context that would resolve the argument spelling is bound; the fourth is the
namespace-resolution refactor that would stop the registries drifting.

## Members

| Order | Item | Status | Shape |
|-------|------|--------|-------|
| 1 | `p3/unique-pointer-spelling-with-unbound-type-parameter` | READY | both `ParseDeclarationSpecifiers` gates rewrite `unique T*` to `unique__T` and queue `unique<T>` while `T` is unbound. Defer like the explicit `unique<T>` spelling (`CanDesugarUniqueTypeArg`). Legs in `Test/test_move.cb`. |
| 2 | `p3/function-scope-alias-not-usable-as-generic-argument` | READY (hypothesis, verify first) | `Box<MyInt>` mangled before the body's `using` frame is active in the scanner. Push the alias frame when the scanner enters the body. Leg in `Test/test_generics.cb`. Verify: where is `Box<MyInt>` first mangled vs `ScanUsingDeclaration`. |
| 3 | `p3/generic-array-view-arg-diagnostic-points-into-core` | READY (diagnostic only) | `list<int[]>` reports `cannot find the type 'int[]'` at the substituted core line under the user file name. Report at the ARGUMENT site when substitution forms `T[]*`. Do NOT block `T[]` as an argument generally (`Box<int[]>` works). |
| 4 | `p3/centralize-scoped-registry-resolution` | REFACTOR, last | typed helpers: register under scoped key; find-first-visible by outward walk with `forceRoot`. `FuncPtrStructCandidates` asks the scoped resolver first, tail scan only as fallback. Q12/Q15 tests unchanged. |

## Shared root cause (members 1-3)

`ForwardRefScanner` computes the instance name (and for 1 also queues the instantiation) from the
argument spelling as written, with no function alias frame and no "is this an unbound template
parameter" check. The explicit-generic path already defers to instantiation; the pointer sugar and
the alias path do not.

## Sequencing

1 and 2 touch the same scanner queueing path - do them in one round, 1 first (smaller, clearer
root cause). 3 is a separate attribution change at instantiation error time; independent. 4 last.

## Constraints

- Any type-parsing change goes in BOTH `ParseDeclarationSpecifiers` copies.
- Grammar stays predicate-free; nothing here should need `CFlat.g4`.
- No new test files; extend `test_generics.cb` / `test_move.cb`.
- New diagnostic (member 3) = `LogError` format string only; locales regenerate.

## Adjacent

- Round-1 q12 (generics/mangling, landed `eddf712`) and round-1 q03 (namespace/alias, `dfa443a`) - the
  rulings that produced `ScopedNameCandidates`, which member 4 generalizes.
- [[invertible-mangling-ruling]]: instance names must stay demanglable; member 1's deferred name
  must go through the same `$` scheme.
