# A shape-mismatched function-pointer argument still binds silently and jumps through it

Filed 2026-07-31 by the round-2 review of `function-pointer-overloads-collide-in-mangling`.
**Pre-existing**, verified on the master binary at `8c29ca7` and unchanged by the fix that
landed as `4000fa1`.

Severity: **SILENT MISCOMPILE, then SIGBUS (exit 138).** No diagnostic at any point.

## Repro

```cflat
import "function.cb";
int dbl(int x) { return x * 2; }
function<int(int)> g = dbl;
int only(function<int(int)> f) { return f(3); }
extern int main() { printf("only=%d\n", only(&g)); return 0; }
```
Observed on `8c29ca7` and on `4000fa1`: **exit 138**, no output, no diagnostic. A
`function<T>*` (pointer to the thin pointer) is passed where a plain `function<T>` value is
expected; the address of the variable is called as if it were the code address.

## Root cause

`4000fa1` taught the overload scorer to compare a three-state indirection SHAPE - array /
pointer / plain value - via `FunctionPointerShapeOf` in `cflat/LLVMBackend.h`. A disagreeing
shape now scores `1` (implicit) instead of `0` (perfect), which is what makes `function<T>[]`
and `function<T>*` overloads resolve correctly.

But scoring `1` still leaves `promotionMatch` TRUE. So when no better-shaped arm exists, the
mismatched arm is still selected and lowered, exactly as before - the scorer now KNOWS the
shape disagrees and declines to act on it. With two arms this is merely a ranking; with one
arm it is a silent bind into a miscompile.

## Fix direction

The shape knowledge added by `4000fa1` is the natural hook: when the SELECTED candidate has a
non-zero function-pointer shape mismatch against the argument, emit a proper
`LogErrorContext` instead of lowering. Word it after the existing closure-parameter guard:
`cannot pass ... to closure parameter '<name>': ... a data pointer would be called as code.`

**Guard polarity is load-bearing:** reject only a PROVABLE shape disagreement (the scorer
already computed it), and only for a function-pointer parameter. Do not extend this to
arguments whose shape could not be determined - `FunctionPointerShapeOf` returns `0` for
anything it cannot classify, and `0 == 0` must stay accepted.

Note the closely related `closure-param-accepts-data-pointer` covers a DIFFERENT hole in the
same area (a plain data pointer widening into a fat closure slot). Check whether that work
subsumes this one before starting; if the accept-set gate lands there first, this may reduce
to a test.

## Test coverage

None. Wants a leg in `Test/errors/err_data_pointer_to_closure_param.cb` once fixed.

Related: [[closure-param-accepts-data-pointer]],
[[funcptr-fixed-array-vs-view-overloads-collide]], [[interface-issue-queue]]
