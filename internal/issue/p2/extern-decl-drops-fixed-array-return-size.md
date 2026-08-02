# `extern` declaration silently drops a fixed-array return size

Filed 2026-08-02 on `fix/array-storage` (round 2), while closing the by-value
fixed-array return axis.

Severity: SILENT WRONG ABI. No diagnostic, no crash.

## Repro

Measured on `ca5a02a` Release AND on `fix/array-storage`, identical on both:

```cflat
extern char[8] extmk();
extern int main(){ printf("ok\n"); return 0; }
```

-> compiles rc 0, links, runs rc 0, prints `ok`.

The DEFINITION form is rejected on `fix/array-storage`:

```cflat
char[8] mk() { char[8] b = default; return b; }
// -> function 'mk' cannot return the fixed array 'char[N]' by value
```

## Root cause

The by-value fixed-array return reject added on `fix/array-storage` lives on the
function DEFINITION path in `MainListener.h` (the arm that has already resolved
`func->compoundStatement()`), keyed on `returnType.ArraySize` / `AliasArraySize`.
An `extern` prototype with no body never reaches it, so the `[8]` is dropped exactly
as it used to be for definitions: the declaration binds to a symbol returning a
single `char`, and any call site would read one byte where the callee wrote eight.

Not a regression - it behaves the same on `ca5a02a`. It is the one remaining spelling
of an axis that is otherwise closed.

## Fix direction

Apply the same `returnType.ArraySize != nullptr || returnType.AliasArraySize > 0`
reject (excluding `IsArrayView`, `IsSimd`, `Pointer`) on the extern/prototype path.
Verify against a real C prototype first: C forbids returning an array, so no valid
`.h` binding should trip it, but `--c-include` auto-extern registration goes through
its own path and must be checked before the guard is added, not after.
