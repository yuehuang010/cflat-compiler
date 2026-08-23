# p4: global array-VIEW initializer from literals (ruling needed)

Moved from p3 2026-08-23. The fixed-size `string[2] g = { ... }` form already works; this is the
convenience of omitting the count on a global view, plus globals of ctor-needing element types.

- Proposed: `string[] g_symbols = { "AAPL", "MSFT" };` at file scope infers a 2-element backing
  array and binds the view to it, exactly as the local form does.
- Alternative: keep rejecting and require the counted form (document it); or accept only for
  element types that fold to constants and keep rejecting ctor-needing element types.
- Acceptance: global view length/indexing match the counted form; ctor-needing element types
  either work via a static initializer or get a clear diagnostic; leg in Test/test_core.cb next
  to `global string array element 0/1`.

---

# Residual: global array-VIEW initializers, and global arrays of ctor-needing element types

The `string[N]` half of this issue is FIXED (p3 bundle, off `819848e`): a global
`string[2] g = { "AAPL", "MSFT" };` now folds each literal element into the borrowed
`{ptr,len}` constant a scalar `string g = "AAPL";` global already used
(`MainListener_Expressions.cpp`, `EmitGlobalFixedArrayInit`). Covered by
`Test/test_core.cb` (`global string array element 0/1`).

## What is left

1. The array-VIEW spelling is still rejected outright:

```cflat
string[] g_symbols = { "AAPL", "MSFT" };
```
```
array-view initializer '= {}' is not allowed at global scope
```

A view needs backing storage the global has nowhere to put; the fix would be to synthesize a
hidden fixed array and point the view at it. Not attempted - it is a storage-model decision.

2. A global array whose element type needs a RUNTIME constructor is still rejected, but the
   message now names the real cause instead of blaming the elements:

```
a global array of '<T>' can only be initialised from compile-time constants; this element needs
a runtime constructor - assign it at startup, or use 'const char*[]' for a static table of
literals
```

Emitting a real static initializer per element (the "preferred" direction in the original
filing) is still open, and is the same machinery a global default-constructed struct would
need.
