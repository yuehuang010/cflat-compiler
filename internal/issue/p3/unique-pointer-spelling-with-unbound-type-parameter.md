# `unique T*` with an UNBOUND type parameter in a generic signature fails: "cannot find the type 'T'"

Found 2026-09-02 while verifying the return-type desugar on the unique<T> branch. Pre-existing
for parameters (same error on the pre-change binary); the return-type desugar extends it to
returns, where the raw spelling used to compile.

## Repro

```cflat
struct R { int v = 0; };
int take<T>(unique T* p) { return p->v + 1; }            // g1d: cannot find the type 'T'
unique T* mkg<T>() { unique T* r = new T; return move r; } // g1a: cannot find the type 'T'
int main() { return take<R>(new R); }
```

`unique<T> mkg<T>()` (the explicit spelling) compiles and runs (scratch/rt/r/g1b.cb), so the
two spellings are not yet equivalent inside a template signature. Probes scratch/rt/r/g1a,
g1d, g1e on the branch. No test or core file uses the pointer spelling with an unbound
parameter, which is why the suite is silent.

## Root cause

Both ParseDeclarationSpecifiers gates rewrite `unique T*` to `unique__T` and queue the
`unique<T>` instantiation at template-scan time, before `T` is bound; the explicit `unique<T>`
spelling goes through the generic type-argument path (MainListener_Declarations.cpp ~205-235,
CanDesugarUniqueTypeArg) which defers to instantiation.

## Fix direction

When the pointee names an unbound type parameter of the enclosing template, do not queue;
record the desugared name and let instantiation substitute it (the same deferral the explicit
spelling gets). Add positive legs for the parameter and return forms to Test/test_move.cb.
