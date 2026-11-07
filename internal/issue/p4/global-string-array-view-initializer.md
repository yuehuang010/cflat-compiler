# p4: global arrays with runtime-constructed elements

The file-scope array-view initializer half was fixed in GVIEW: `T[] g = { ... };`
now infers hidden backing storage for compile-time-foldable elements. Its regression
coverage is in `Test/test_core.cb`.

## Repro

```cflat
struct NeedsInit { int value; NeedsInit() { value = 7; } };
NeedsInit[1] values = { {} };
```

## Root cause

`EmitGlobalFixedArrayInit` only creates LLVM constant initializers. It cannot run a
constructor as part of static initialization, and currently rejects ctor-needing
elements with a located diagnostic.

## Fix direction

Keep the compile-time-constant path. If this is implemented, add explicit startup
construction for fixed-size global arrays and preserve declaration ordering and
thread-local semantics. Do not add runtime initialization to file-scope array views;
those accept only foldable elements.
