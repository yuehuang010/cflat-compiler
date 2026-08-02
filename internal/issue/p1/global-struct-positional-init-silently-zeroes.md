# A positional brace initializer on a GLOBAL struct silently zeroes it

Filed 2026-08-02 on `fix/array-storage` (round 2), found while sweeping legal
aggregate globals against the widened global-initializer guard.

Severity: SILENT MISCOMPILE. No diagnostic, wrong values, exit 0.

## Repro

Measured on `ca5a02a` Release AND on `fix/array-storage` - identical on both, so
this is PRE-EXISTING and not a regression.

```cflat
struct S { int a; int b; };
S gs1 = {1,2};
extern int main(){ printf("a=%d b=%d\n", gs1.a, gs1.b); return 0; }
```

-> compiles rc 0, runs rc 0, prints `a=0 b=0`.

The identical LOCAL spelling is REJECTED, with a clear message:

```cflat
extern int main(){ S ls = {1,2}; return ls.a; }
// -> positional initializers are not supported for struct type 'S'; use 'field = value'
```

So the two scopes disagree about the same construct: the local says the form does
not exist, the global accepts it and throws the values away.

## Root cause

Not diagnosed. The local declarator path runs the positional-initializer check that
produces the message above; the global declarator path does not reach it, and the
brace list evidently does not survive into the global's `llvm::Constant` initializer
(the global lands as a zeroinitializer). Note that a positional brace list on a
global fixed ARRAY (`int[4] g = {10,20,30,40};`) DOES work and is covered by value
legs in `test_basic.cb` - it is specifically the STRUCT element case that is lost.

## Fix direction

Decide which scope is right, then make them agree. Either:

1. Route the global declarator through the same positional-initializer check, so
   `S gs1 = {1,2};` gets the local's diagnostic (the low-risk option - it only turns
   a silent wrong value into the message the language already gives elsewhere); or
2. Implement positional struct init in both scopes.

Whichever is chosen, the arrays-of-structs and nested-struct cases need enumerating
in the same pass, and a value leg must go in `test_basic.cb`'s `testFixedArrays` /
the struct tests rather than a new file.
