# `nullptr` into a `function<T>` VALUE parameter compiles clean and SIGSEGVs on call

Filed 2026-08-06 during Phase A of `fix/shape-arg` (the shape-mismatched funcptr argument fix),
while enumerating the nullptr x {value, pointer, view} cross-product for `FunctionPointerShapeOf`.

Severity: LOW / UNDIAGNOSED CRASH, but likely BY DESIGN - flagged for a decision, not a fix.

## Repro

```cflat
import "function.cb";
int only(function<int(int)> f) { return f(3); }
extern int main() { printf("r=%d\n", only(nullptr)); return 0; }
```

Compiles clean, exit 139 (SIGSEGV) at `f(3)`. No diagnostic at any point.

## Why this is probably NOT the shape-mismatch bug

`nullptr` into a POINTER (`function<T>*`) or VIEW (`function<T>[]`) parameter compiles and runs
correctly (the pointer/view is only compared to `nullptr`, never called) - both are exercised as
accept-set legs in `Test/test_function_ptr.cb::testFuncPtrShapeGateAccepts`
(`shape_nullptr_into_pointer`, `shape_nullptr_into_view`). Those two are FunctionPointerShapeOf
shape 1 and 2, where a null value is inert.

Passing `nullptr` into a VALUE (shape 0) parameter is different in kind: the value itself IS the
callable, so a null `function<T>` and then calling it is closer to `int* p = nullptr; *p;` -
a null dereference at the CALL site, not an indirection-shape confusion. `FunctionPointerShapeOf`
computes shape 0 for `nullptr` on both sides (arg and param), so this is not a shape disagreement
the scorer/gate added by `fix/shape-arg` can see or should reject - forcing it into that gate
would conflate two different questions.

## Open question

Should assigning/passing `nullptr` to a non-pointer `function<T>` be a COMPILE-TIME error instead
(the language already has null-safety features - nullable `?` - so an unchecked null function
value may be an oversight, not an intentional escape hatch)? Or is this accepted as the same
class of runtime UB as any other null-pointer call, matching C's `void (*fp)(void) = NULL; fp();`?
If the latter, this file should be closed as WORKING AS INTENDED rather than fixed.

## Fix direction (if pursued)

If null function values are meant to be disallowed without a nullable annotation, reject at the
same construction sites `err_data_pointer_to_closure_param.cb` already covers (decl-init,
assignment, field, param default, array element) for a `function<T>` (non-pointer) destination
fed a literal `nullptr`. If accepted as intentional UB, no compiler change - just note the
decision in `internal/issue/interface-issue-queue.md` so it is not re-investigated.

## Related

[[interface-issue-queue]]
