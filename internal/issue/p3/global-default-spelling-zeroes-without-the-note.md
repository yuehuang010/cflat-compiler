# A global spelled `= default` / `= { }` zeroes SILENTLY when its construction does not fold

Bucket: full mode (needs a ruling on diagnostic volume before it can land). Filed 2026-09-05 as a
side finding of fix/string-field-default.

## Summary

Three spellings of the same global construction take different diagnostic paths:

- `T g;` (no initializer) - `MainListener_Declarations.cpp:5525-5535` folds, and on failure queues
  `pendingGlobalDefaultConstruction`, so `ResolvePendingGlobalDefaultConstructions` retries after
  the module walk and, if it still does not fold, emits the located note
  "(T) global is zero-initialized: its default construction could not be reduced to a compile-time
  constant here."
- `T g = default;` (`:5454`) and `T g = { };` (`:5465`) go through `GenerateDefaultValue`, whose
  global branch falls back to `llvm::Constant::getNullValue` with NO note and NO end-of-module
  retry. The global reads zero and nothing says so.

## Repro

```
const char* gPtr = "rt";
struct R1 { string s = gPtr; int n = 1; };
R1 gBare;                 // note printed, reads 0
R1 gDefault = default;    // NO note, reads 0
```

Both globals are zeroed; only the first is diagnosed. `Test/test_initializer_list.cb:85`
(`FbEq fbGlobal = default;   // globals skip the ctor - still zero`) is an in-tree instance.

## Root cause

`GenerateDefaultValue`'s global arm (`MainListener_Declarations.cpp:1620-1626`) declines silently;
only the no-initializer declarator arm knows about the pending-retry list.

## Fix direction and why it was not done in fix/string-field-default

Queueing the retry from the `= default` and EMPTY-brace arms is a ~10-line change (measured: a
`GlobalDefaultNeedsFoldRetry` helper gated on `global_scope`, a non-pointer non-array struct type
with a ctor, and an all-zero result). It works, but it surfaces roughly 20 pre-existing silent
zeroes across `Test/` and `example/` in one step - `err_constraints.cb`, `err_lambda_array_view.cb`,
`test_allocators.cb`, `test_collection_leaks.cb`, `test_core.cb`, `test_initializer_list.cb`,
`test_move.cb` and more. Every one of those notes is TRUE, but the volume is a diagnostics policy
change, not a bug fix, so it needs a maintainer ruling first: emit the note everywhere, or only
where a field initializer was actually dropped. A non-empty brace list (`= { a = 1 }`) must NOT be
queued - its field values are written into the same constant afterwards and the retry's
`setInitializer` would clobber them.
