# q06 - BATCH: generics and name-resolution one-site fixes

Ships as ONE worktree (`fix/batch-generics`), one commit, one scoped review round. Three
members with a verified site each in the pre-scan / instantiation / scoped-lookup code; all
pre-existing on master; none adds a rejection. The two items in the same area that do NOT
qualify (a ruling, a refactor) are listed below so they are not pulled in.

## Members

| # | Item | Site | Leg |
|---|------|------|-----|
| 1 | `p3/namespace-alias-does-not-reach-generic-template-base` | `ResolveGenericTemplateBase` / `ResolveTypeArgBaseName` (LLVMBackend_Interfaces.cpp): `ResolveFirstComponentAlias = true`; queued key must be canonical in both pre-scans | `IN.GBox<int>` leg in `testNamespaceTypeShadowing`; `--symbol-dump` shows the canonical mangled name |
| 2 | `p3/generic-function-view-arg-diagnostic-points-into-template` | generic-function instantiation path (MainListener_Generics.cpp) publishes `gts.activeInstantiationOrigin` like `ProcessPendingInstantiations` | leg in Test/errors/err_generic_array_view_arg.cb for `f<int[]>()` reporting the call site |
| 3 | `p3/if-const-scan-folder-misses-sizeof-enum-cast` | `FoldCompileTimeInt` leaves: `sizeof` (type-size query through the existing alias/struct lookup), C-cast as no-op on integer constants; the enum leaf waits for q05 | `sizeof`-decided and cast-decided arms in `testFunctionScopeAliasAsGenericArgument`; enum arm added when q05 lands |

## Not in the batch

- `p3/generic-pointer-to-view-field-collapses-to-raw-pointer` - RULING (error vs decay);
  full mode once ruled, reuses member 2's origin plumbing.
- `p3/funcptr-signature-component-lacks-declaring-scope` - REFACTOR, full mode, last;
  scoped-first-at-comparison-time was measured and false-rejects two legs, do not retry.

## Constraints

- The folder is shared by both pre-scans; change the helper, not one scanner.
- Grammar stays predicate-free.
