# `function<T>[N]` and `function<T>[]` overloads collide silently - the last one wins

Filed 2026-07-31 by the round-2 review of `function-pointer-overloads-collide-in-mangling`.
Behaviour on `4000fa1`; on `8c29ca7` the same program SEGFAULTS, so this is strictly an
improvement over master, not a regression.

Severity: silent overload loss, no diagnostic. Low - the two spellings are arguably the same
parameter type, which is why this is P3 rather than P1.

## Repro

```cflat
import "function.cb";
int dbl(int x) { return x * 2; }
int pick(function<int(int)>[3] fns) { return 10 + fns[0](1); }
int pick(function<int(int)>[] fns)  { return 20 + fns[0](1); }
extern int main()
{
    function<int(int)>[3] arr;
    arr[0] = dbl; arr[1] = dbl; arr[2] = dbl;
    printf("pick=%d\n", pick(arr));
    return 0;
}
```
On `4000fa1`: prints `22` - the `[]` arm (`20 + dbl(1)`), silently, with no diagnostic that the
`[N]` declaration was discarded. The `[3]` arm would have printed `12`. On `8c29ca7`: segfault.

## Root cause

Two independent places collapse the fixed and view spellings onto one key, and BOTH would have
to change to separate them:

- `ToUniqueString()` (`cflat/LLVMBackend.h` ~713) maps both to the `funcptrArr` prefix, so the
  second declaration is absorbed by the first at registration - the same mechanism that made
  `function<T>` and `function<T>*` collide before `4000fa1`.
- `FunctionPointerShapeOf()` (`cflat/LLVMBackend.h` ~16723) maps both to shape `2` (array), so
  even with distinct keys the scorer could not rank them apart.

This is deliberate in the second case: `4000fa1` needs a FIXED array argument to bind an
array-view parameter at perfect score, which is the common and correct calling pattern
(`applyAllFuncPtrs(fns, 3, 3)` depends on it). Any separation must preserve that.

## Fix direction

Two acceptable outcomes; the second is cheap and probably right:

1. **Distinguish them** - give the fixed spelling its own mangled key and its own shape state,
   while keeping a fixed-array ARGUMENT binding an array-view PARAMETER at perfect score. The
   size would then also need to participate, since `[3]` and `[4]` are different types.
2. **Reject the redefinition** - a `LogErrorContext` when two overloads of one name differ only
   in fixed-vs-view array spelling of a function-pointer parameter, since they are not
   distinguishable at a call site. A clean rejection is acceptable; silently keeping one is not.

Whichever is chosen, `function<T>[N]` and `function<T>[]` must each keep working on their own,
and the `[]`-vs-`*` resolution that `4000fa1` fixed must not regress - it is pinned by
`pickFnPtrShape` / `pickFnPtrShapeRev` in `Test/test_function_ptr.cb`.

## Test coverage

None for the collision. Wants either a value-asserting leg in `Test/test_function_ptr.cb`
(outcome 1) or an `expect_error` leg (outcome 2).

Related: [[shape-mismatched-funcptr-arg-binds-silently]], [[interface-issue-queue]]
