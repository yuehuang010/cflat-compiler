# Casting a generic function instantiation to void* crashes the compiler

Found 2026-08-08 while building the accept/reject matrix for
`fix/cast-launder-occurrence`. Unrelated to that fix - reproduces identically pre- and
post-fix, so it is pre-existing and out of scope there.

## Repro (`scratch/slo_r13_generic_instantiation.cb`, `scratch/slo_r15_generic_cast_only.cb`)

```cflat
import "function.cb";
T gid<T>(T x) { return x; }
int one(void* v) { return 1; }
extern int main(int argc, char** argv)
{
    printf("%d\n", one((void*)gid<double>));
    return 0;
}
```

`x64/Release/cflat slo_r15_generic_cast_only.cb -o out.exe` exits 139 (SIGSEGV) with zero
diagnostic output. The bare (uncast) spelling `one(gid<double>)` is cleanly diagnosed
("'gid' is a generic function and cannot be used as a function<T> value" or similar), so the
crash is specific to the CAST of a generic instantiation reference, not the bare mention.

## Root cause

Not yet investigated. `ParseCastExpression` (MainListener_Expressions.cpp) presumably calls
`compiler->ParameterStoresData(destTypeName)` -> `RegisterCodeValueDataCast(namedVar.Primary)`
on the cast result of a generic-instantiation reference; the crash likely lives upstream of
that, in how a generic instantiation reference resolves to a `NamedVariable`/`llvm::Value*`
before the cast even runs, since `FunctionPointerShapeOf`/`CreateCast` assume a stable
`llvm::Function*` or ordinary pointer value.

## Fix direction

Not investigated. Start by comparing the `NamedVariable` a generic-instantiation reference
produces (before any cast) against what a named function or `function<>` value produces -
one of those fields is probably null or malformed where a downstream helper assumes it is
populated.
