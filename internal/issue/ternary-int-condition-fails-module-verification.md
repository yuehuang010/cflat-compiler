# '?:' (ternary) with a non-bool (e.g. `int`) condition fails LLVM module verification

Opened while testing deref-of-moved-pointer-across-blocks-not-diagnosed.md. Tripped over
directly while writing a ternary repro for that unrelated issue - not caused or fixed by
it. Filed separately from ternary-arms-evaluated-eagerly-not-short-circuited.md: that
file is about the ARMS being evaluated unconditionally (unsound but well-typed IR); this
one is about the CONDITION operand having the wrong LLVM type outright (a hard compiler
crash via `Error: module verification failed.`, not a silent runtime bug). Different code
path, different failure mode, different fix.

## Summary

```cflat
struct R { int v = 0; };
extern int main() {
    unique R* a = new R();
    int cond = 0;
    unique R* b = cond ? move a : nullptr;
    printf("%d\n", (int)(a == nullptr));
    return 0;
}
```

fails to compile with `Error: module verification failed.` - a hard internal error, not a
clean diagnostic. The same shape with `bool cond = false;` instead of `int cond = 0;`
compiles and runs (see ternary-arms-evaluated-eagerly-not-short-circuited.md for what it
then does).

## Root cause

`ParseConditionalExpression` (MainListener.h) evaluates the condition via
`ParseLogicalOrExpression(logicCtx)`, then passes the raw resulting `TypedValue` straight
into `compiler->CreateSelect(condTv.value, falseValue, trueValue)` with no coercion:

```cpp
auto* selectValue = compiler->CreateSelect(condTv.value, falseValue, trueValue);
```

`LLVMBackend::CreateSelect` is a thin, uncoercing wrapper over
`llvm::IRBuilder::CreateSelect`, which requires its condition operand to be `i1`. An
`int cond` produces an `i32` `condTv.value`; LLVM's `select i32 ... , T, T` is
ill-typed, and the module verifier (run right before codegen finishes) rejects it -
surfacing as the generic, unhelpful `Error: module verification failed.` rather than a
CFlat-level diagnostic naming the actual problem (a non-bool ternary condition).

Contrast `if`/`while`/`for` conditions, which route through whatever helper CFlat uses to
lower a branch condition (`CreateConditionJump` or equivalent) - that path evidently
coerces a non-bool condition to `i1` (e.g. via a `!= 0` compare), since `if (cond)` with
an `int cond` works fine. The ternary path never calls the equivalent coercion before
`CreateSelect`.

## Fix direction

Coerce `condTv.value` to `i1` before `CreateSelect`, the same way the `if`/loop condition
path already does (find and reuse that existing coercion helper rather than duplicating
the "is it already i1, else compare-not-equal-zero" logic). This is a small, self-
contained fix, independent of the eager-arm-evaluation issue above - it does not require
making `?:` branch.
