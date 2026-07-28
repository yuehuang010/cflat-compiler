# Array-view parameters are unconditionally marked 'noalias'

Filed 2026-07-27, found during review of the interface-array-view fix. PRE-EXISTING and
not specific to interfaces - it applies to array views of EVERY element type.

Severity: latent `-O2` MISCOMPILE hazard (undefined behaviour handed to LLVM).

## Repro shape

```cflat
int f(P[] a, P[] b, int n) { ... }
```

emits `ptr noalias %a, ptr noalias %b`. Two array-view parameters that alias each other,
or a view that aliases the array it was taken from, are trivially constructible in
CFlat - nothing in the language forbids passing the same view twice, or passing a view
and the underlying array.

`noalias` is a PROMISE to LLVM that the pointers do not alias. Breaking it is undefined
behaviour: LLVM may reorder or eliminate loads and stores across the two parameters.

## Status

Observed on master `dcb9003`. Both the struct-view and interface-view aliasing cases
currently still produce the correct (aliased) answer at `-O2`, so no live miscompile has
been demonstrated - the optimizer simply has not exploited it yet. That makes this
latent rather than active, but it is exactly the kind of latency that turns into a
miscompile on an LLVM upgrade or a small codegen change.

## Fix direction

Decide the intended language semantics first:

1. If array-view parameters are allowed to alias (the conservative, C-like reading),
   stop emitting `noalias` on them. Cost: some lost optimization on the common
   non-aliasing case.
2. If they are contractually non-aliasing, that must be documented in
   `doc/LANGUAGE.md` and ideally checked - silently promising it to LLVM while the
   language never states it is the worst of both.

Option 1 is the safe default. Find the attribute application site (the array-view
argument path in `cflat/LLVMBackend.h`, near the array-view parameter setup) and gate
it, or drop it outright.

Note: the interface-array-view fix made `IFace[]` parameters lower as real pointers, so
they now carry this attribute too. That fix did not introduce the hazard - it merely
made one more element type reach it.
