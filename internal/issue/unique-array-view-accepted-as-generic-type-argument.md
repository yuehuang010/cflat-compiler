# 'unique T[]' is accepted as a generic type argument but rejected as a declaration

Filed 2026-07-27, found during review of the interface-array-view fix. PRE-EXISTING:
identical on master `dcb9003` and on that fix branch.

Severity: inconsistent accept set. No miscompile demonstrated.

## Repro

```cflat
interface IS { int f(); };
// rejected at the declaration (correct - a view does not own its buffer):
unique IS[] local = ...;      // error: 'unique' on 'IS[]': array views are not supported

// accepted as a generic type argument:
struct Box<T> { T v = default; };
Box<unique IS[]> b;           // compiles, runs, one alloc, correct result, no crash
```

`Box<unique int[]>` IS correctly rejected. Only the interface-element form slips
through.

## Root cause

`cflat/MainListener.h:2734` checks

```cpp
if (!hasPointer && !IsInterfaceType(uniqueBase))
```

without excluding `hasArrayView`, so an interface element type short-circuits the
rejection that the non-interface path gets.

## Why it is not currently dangerous

The field-destructor arms at `LLVMBackend.h:4409` and `:4422` both still carry an
explicit `!f.IsArrayView`, so a `unique` view smuggled in this way does not reach the
owning-value cleanup that would double-free or mis-destruct it. It simply behaves as a
borrowed view.

## Fix direction

Add `hasArrayView` to the condition at `MainListener.h:2734` so the generic-argument
path rejects with the same message the declaration path uses:
`'unique' on 'IS[]': array views are not supported - a view does not own its buffer`.

Check the same shape for `unique` on an array view in other type-argument positions
(function return type, nested generic, container element) before closing - the
declaration rule and the type-argument rule should agree everywhere.

Related: the declaration-level rule was tightened in the interface-array-view fix; this
is the one position it did not reach.
