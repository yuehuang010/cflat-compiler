# Calling a GENERIC function that returns `function<>` corrupts the ENCLOSING function's return

Filed 2026-08-01, found while enumerating the spelling axes of
`funcptr-call-result-into-closure-param-garbage` (fixed and deleted; see the
"fix/funcptr-callresult" landed record in [[interface-issue-queue]]). **Pre-existing** - verified
on the master binary at `3b6e3e8`, and unrelated to that fix, which only guards a call ARGUMENT
re-resolve.

Severity: **P2 - legal code rejected, with a P1-grade diagnostic.** The compiler emits a raw
LLVM verifier dump and `Error: module verification failed.` with NO source location - the
message names `%thinret`, which is meaningless to a user. Per CLAUDE.md's convention an LLVM
verifier failure like this should become a proper compiler error at minimum, and the shape here
is a legal program that should simply compile.

> Bucketed P1 when first filed, moved to P2 the same day. The deciding evidence: the pointer
> case below COMPILES AND RETURNS CORRECTLY, so there is no silent-wrong-value shape here. Every
> failing spelling fails LOUDLY at compile time, which is the safe side and ranks below the
> silent-abort / wrong-value P1s.

## Repro

```cflat
import "function.cb";
int dbl(int x) { return x * 2; }
function<int(int)> mk<T>(T seed) { return dbl; }
int probe() { function<int(int)> g = mk<int>(1); return g(5); }
extern int main() { printf("%d\n", probe()); return 0; }
```

```
  %thinret = bitcast i32 %3 to ptr
Function return type does not match operand type of return inst!
  ret ptr %thinret
 i32
Error: module verification failed.
```

The ENCLOSING function's `return` is lowered as if IT returned a thin function pointer: the
returned value is bitcast to `ptr` while the function's LLVM return type is still `i32`.

## Axes enumerated on `3b6e3e8` - the trigger is narrower AND wider than first reported

The original hand-off claimed the enclosing return type had to be `bool`/non-`main` and that
"`main` escapes by luck". **Both halves of that are wrong.** Measured:

| Spelling | Result on `3b6e3e8` |
|---|---|
| enclosing returns `bool` | FAILS - `bitcast i1 %6 to ptr` |
| enclosing returns `int` | FAILS - `bitcast i32 %3 to ptr` |
| **the enclosing function IS `main`** | **FAILS** - `main` does NOT escape |
| enclosing returns `void` | compiles, runs, prints `10`, exit 0 |
| **enclosing returns `int*`** | **compiles, runs, returns the right pointer, exit 0** |
| producer is NON-generic (`function<int(int)> mk(int)`) | compiles, runs, prints `10`, exit 0 |
| call result never used (`mk<int>(1);` as a statement) | FAILS |
| result bound but never called (`... g = mk<int>(1); return 7;`) | FAILS |

So the two conditions are: **(1) the callee is a GENERIC function returning `function<>`, and
(2) the enclosing function returns a non-`void`, NON-POINTER type.** Nothing about how (or
whether) the result is used matters - merely CALLING the generic producer anywhere in the body
poisons that body's `return`. A `void` enclosing function escapes because it has no return value
to coerce; a POINTER-returning one escapes because the bogus coercion is a ptr-to-ptr bitcast,
which is a no-op under opaque pointers.

That pointer row is why this is P2 and not P1: the coercion fires there too, and produces
CORRECT code by accident. There is no spelling in which it produces a wrong value silently -
it either no-ops or fails the verifier.

## Root cause direction - LEAD, not diagnosed

The caller's `return` is being routed through `CoerceToFuncPtrReturn`
(`cflat/LLVMBackend.h` ~9886). The reading to verify: monomorphizing `mk<T>` sets some
"current function returns a thin funcptr" state that is not scoped to the instantiation, so it
is still live when the ENCLOSING function's `return` is lowered. That would explain why the
result being unused makes no difference - the damage is done by lowering the call at all - and
why `void` escapes.

Confirm against the IR before editing. Check whether the state is saved/restored around
generic instantiation, and note the repo lesson that `LogError` throws, so any hand-rolled
save/restore must be RAII to survive an unwind.

## A SECOND defect on the neighbouring spelling - do not conflate

A generic producer with NO value parameter fails differently, with a false diagnostic:

```cflat
function<int(int)> mk<T>() { return dbl; }
int probe() { function<int(int)> g = mk<int>(); return g(5); }
```
```
g_expl.cb(4,37): cannot assign a struct value to a pointer variable - use getPtr() or take the
address with '&'
```

This is a different failure (a false rejection at the assignment, not a verifier failure at the
return) and may or may not share a root. Probe it separately; do not assume one fix closes both.

## Test coverage

None. `Test/test_function_ptr.cb` has no generic-producer leg - deliberately, because this bug
blocks writing one. Add a value-asserting leg there once fixed.

Related: [[interface-issue-queue]]
