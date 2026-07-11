# Generic method on a non-generic class cannot see `this` / its fields

Found: 2026-07-11 (while implementing memory-mapped files - see the G8
section of internal/plan/hpc-gaps.md)
Status: OPEN - worked around at the one call site that hit it

## Summary

A method that introduces its OWN type parameter on a class that is NOT
itself generic fails to compile: field references in the body do not
resolve, reporting "Undefined variable <field>". The receiver is
effectively not bound inside such a method.

Generic methods on GENERIC classes work, and non-generic methods on
non-generic classes work. It is specifically the
non-generic-class + generic-method combination that breaks.

## Repro

```cflat
class Holder
{
    i64 _n = 0;
    T get<T>() { return (T)_n; }    // <-- "Undefined variable _n."
};

extern int main()
{
    Holder h;
    h._n = 7;
    printf("%d\n", (int)h.get<int>());
    return 0;
}
```

Actual: `probe.cb(4,27): Undefined variable _n.`
Expected: prints 7.

## Root cause

Not yet diagnosed. The shape of the failure (the field, not the method,
is what fails to resolve) points at the monomorphized instance of the
method being registered/emitted without the enclosing class's `this`
scope pushed - i.e. it is treated like a free generic function that
happens to be namespaced under the class, rather than a member. The two
`ParseDeclarationSpecifiers` copies and the generic-instantiation path in
`ForwardRefScanner` are where to start.

## Workaround in use

Take the receiver explicitly as a parameter and make the method static -
the same idiom `simd<T,N>.load` / `.store` already use:

```cflat
static span<T> asSpan<T>(MappedFile* f) { ... f->_len ... }   // works
```

`core/filesystem.cb`'s `MappedFile.asSpan<T>(f)` ships in this shape for
exactly this reason. If this issue is fixed, that method should move back
to the instance spelling `f.asSpan<T>()`, which is what the G8 plan
originally specified and is the better API.

## Fix direction

Bind the enclosing class scope (`this` + field lookup) when instantiating
a generic method whose owner is a plain class. Worth a regression test in
an existing generics test file once fixed - do NOT add a new test file.
