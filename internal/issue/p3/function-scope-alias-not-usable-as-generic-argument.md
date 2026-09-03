# Function-scope `using` alias cannot be used as a generic type argument

## Summary

A `using` alias declared inside a function body is not visible when it is used as a generic
type argument in the same body. File-scope aliases work. Pre-existing: reproduces on the
main-checkout binary from 2026-08-31 (before scoped aliases landed), so the alias scope stack
did not cause it.

## Repro

```cflat
struct Box<T> { T v = default; };
int main() { using MyInt = int; Box<MyInt> b; b.v = 3; return b.v; }
// (1,16): cannot find the type 'MyInt'
```

`using MyInt = int;` at file scope with the same body compiles and runs.

## Root cause (hypothesis, not yet verified)

Generic instantiations are queued by the pre-scan (ForwardRefScanner /
ScanAndQueueGenericTypeUses) before the function body is emitted, and the type-argument spelling
is resolved through ResolveManglingAlias / ResolveTypeAlias while no function alias frame is
active - the scanner sees the `using` only when it walks the body's block items, after the
instantiation name was computed. Verify by checking where `Box<MyInt>` is first mangled
relative to ScanUsingDeclaration for the block item.

## Fix direction

Resolve type arguments with the enclosing function's alias frame active in the scanner's
queueing path (push the frame when the scanner enters the body, as the main pass does), so the
instance name is `Box$int` in both passes. Add a leg to Test/test_generics.cb.
