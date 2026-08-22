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
