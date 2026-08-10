# A void method call used as a CHAIN RECEIVER silently falls back to the original receiver

Filed 2026-08-09 from the fix/dvoid review. Pre-existing: measured identical on base `43ccb90`
and on the fix branch. Not caught by the direct-void-consume gate because that gate inspects the
whole postfixExpression's FINAL result; an intermediate void link never reaches it.

Severity: wrong-code (silent misbehaviour, rc 0) - the program compiles and runs, calling the
next method on the wrong object with no diagnostic.

## Repro

```cflat
struct B { void f() { } int g() { return 7; } };
extern int main() {
    B b;
    int r = b.f().g();   // rc 0 on both binaries; g() runs on b
    printf("r=%d\n", r);
    return 0;
}
```

`--no-opt` IR shows `call void @_f_(ptr %b)` followed by `call i32 @_g_(ptr %b)` - the void
result of `f()` is discarded and the chain re-uses `%b` as the receiver for `g()`.

## Root cause

Inside ParsePostfixExpressionInner, each chain link's result becomes the next link's receiver;
when the link's call returns void the result NamedVariable is empty and the receiver simply stays
what it was. Nothing diagnoses "member access on a void call result".

## Fix direction

At the point where a chain link's result is adopted as the next receiver, reject when the link's
resolved return type is void (alias-resolved, same rule as DiagnoseVoidResultConsumed) with a
located error naming the void method. This is INSIDE the chain walk, not at the function exit, so
the existing wrapper gate cannot see it - a small check at the link-adoption site is expected.
