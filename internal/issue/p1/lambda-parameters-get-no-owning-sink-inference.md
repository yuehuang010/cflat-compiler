# A lambda's by-value owning parameter is never a sink, so a consuming body double-frees

Filed 2026-08-09 by the work on `fix/parmbrace`. Not a regression of it: measured identical on
`3252c01` and on `fix/parmbrace`, and BOTH store spellings abort, so the assignment oracle that
`fix/parmbrace` mirrored is broken here too.

Severity: double free (abort, rc 134).

## Repro

```cflat
import "function.cb";
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }

extern int main() {
    UBox a = umk(5);
    Lambda<void(UBox)> f = (UBox p) => { UBox[2] d = { p }; printf("elem=%d\n", d[0].item->id); };
    f(a); printf("dtor=%d\n", dtor); return 0;
}
```

Measured (`scratch/pb_41_lambda.cb`, `pb_41b_lambda_assign.cb`): both the brace element
(`UBox[2] d = { p };`) and the assignment element (`UBox[2] d; d[0] = p;`) print `elem=5`, free
once in the lambda body, then abort on the caller's `a` - rc 134. The identical FUNCTION spelling
is rc 0 on `fix/parmbrace` (`scratch/pb_01_brace.cb`, `pb_02_assign.cb`).

## Root cause

`ApplyOwningSinkInference` (`cflat/MainListener.h`) is only called on a
`FunctionDefinitionContext` - from `ForwardRefScanner::ScanFunctionDefinition`,
`MainListener::ParseFunctionDefinition`, and the two generic-instantiation sites in
`MainListener_Generics.cpp`. A lambda literal is a `LambdaExpressionContext` and is lowered on a
different path, so its parameter list never gets the inference. The body still CONSUMES the
parameter (the element arms are shared), so the callee frees a resource the caller still owns.

`CollectConsumedStoreNames` deliberately does NOT descend into a nested lambda (it is a different
scope), which is correct - the gap is that the lambda's own parameter list is never scanned as its
own function.

## Fix direction

Run `ApplyOwningSinkInference` over a lambda literal's parameter list against its own body, and
make the indirect call site (`MainListener_PostfixExpression.cpp` ~2971, which already reads
`funcPtrTV.FuncPtrParams[i].IsMove`) honour the inferred sink the way `ApplyMoveParamTransfer`
does for a direct call. Note the `Lambda<...>` TYPE also has to carry the per-param sink flags, or
a lambda assigned through a declared variable type will lose them - the existing per-param
`IsMove` agreement check on funcptr-to-funcptr assignment
(`MainListener_Expressions.cpp` ~1507) is the precedent to extend.
