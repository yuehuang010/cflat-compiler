# `return default;` for a struct ignores field initializers

Filed 2026-08-21 while verifying the p2 "default as an expression" fix
(commit 3e6162a). Reproduces on the current tree, including after that fix -
adjacent gap, not caused by it.

## Repro

```cflat
struct P { int a = 1; int b = 4; }

P f() { return default; }

int main()
{
    P y = default;
    P z = f();
    printf("y: %d %d\n", y.a, y.b);   // 1 4
    printf("z: %d %d\n", z.a, z.b);   // 0 0 (wrong - should be 1 4)
    return 0;
}
```

`P y = default;` correctly runs the field initializers (1, 4). `return
default;` from a function returning `P` instead produces a zeroed struct
(0, 0).

## Root cause guess

`default` as a variable initializer (`P y = default;`) goes through the
declaration-init path, which calls whatever helper applies per-field
initializer expressions (`a = 1`, `b = 4`) after allocating storage - likely
in `MainListener_Declarations.cpp` near `ParseDeclaration`. `return default;`
instead goes through the return-statement codegen path, which most likely
lowers `default` for a struct return type as a flat `memset`/zero-init of
the sret buffer (or a zeroed aggregate constant) without calling that same
per-field-initializer helper.

Look for the `default` keyword's expression-codegen entry point (probably
shared as `Compiler()->EmitDefaultValue(...)` or similar) and check whether
it takes two different code paths depending on whether it is reached via
declaration-init vs. return-statement, or whether the return path bypasses
the shared helper entirely (e.g. via `ReturnStatement` handling in
`LLVMBackend_ControlFlowAndFunctions.cpp` calling a raw zero-init instead of
the struct's default-initializer emitter).

## Fix direction

Make `return default;` for a struct-typed return route through the same
default-value construction used by `T x = default;` (the one that walks the
struct's fields and applies each field's initializer expression, recursing
into nested structs), instead of a flat zero/memset. The sret buffer should
be filled by that shared emitter rather than zeroed and left alone.

## Regression test

Add a leg to an existing struct-default test (e.g. wherever `= default;` on
a struct with field initializers is already tested) with a function that
`return default;`s such a struct, asserting the field values match their
initializers rather than zero. Do not add a new test file.
