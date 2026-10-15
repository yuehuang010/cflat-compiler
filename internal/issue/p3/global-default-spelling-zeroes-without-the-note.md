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

2026-10-12 (T136 r3): T136 merged native + C++ global construction into one declaration-ordered initializer, but a
native global with a CFlat ctor body spelled `= default` / `= {}` still reads 0 (clang 5); fixing it flips the frozen
test_initializer_list legs ~588 / ~824, so it stays with this issue.

## Arrays: `S[2] g = default;` zero-fills even a foldable element default (T149 r2, 2026-10-13, master 4fee72142)
`struct S { int x = 9; }; S g4 = default; S[2] g5 = default; S[2] g6;` prints g4.x 9, g5[1].x 0, g6[1].x 9 (clang-style
expectation 9 for all). Same for a nested global `O g = default;` whose field is `A[2] a = default` (Sol T149 r1, arrays.cb).
Root cause (Haiku T149): GenerateDefaultValue skips the array element walk when global_scope is true and returns a null constant.
Fix direction: fold each element's default at file scope (the `S[2] g6;` arm already does). WRONG VALUE, not only a missing note.
Repro: scratch/repro_keep/t149/v4.cb.
2026-10-13 T158: flat and N-D `S[N] g = default` now fold the element default (frozen leg fadGlobal updated 0 -> 14, it had
pinned this wrong value). STILL OPEN: nested `O g = default;` whose field is `S[2] a = default` reads 0 for every field
(even `int z = 1`): the constant folder cannot fold a struct whose ctor writes an array-of-struct field, so the whole global
falls back to zero (second root cause, not one site).
