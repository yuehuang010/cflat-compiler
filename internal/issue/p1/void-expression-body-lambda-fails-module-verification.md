# A `void` expression-body lambda dies in the module verifier with no source location

Filed 2026-08-09 while fixing [[lambda-body-owning-temp-never-destructed]]. Measured on master
`0669ebc` (Release, macOS arm64, warm `--init-local`) and unchanged by that fix.

Severity: **P1 by the "no usable diagnostic" rule** - a raw LLVM verifier dump with no
`file(line,col):` prefix, reachable from three lines of ordinary source.

## Repro

```cflat
int hits = 0;
void bump() { hits = hits + 1; }
extern int main()
{
    Lambda<void()> g = () => bump();
    g();
    return 0;
}
```

```
Module verification failed:
Found return instr that returns non-void in Function of void return type!
  ret void <badref>
 void
Error: module verification failed.
Compilation failed.
```

compile rc 1, no binary. The same shape with a parameter
(`Lambda<void(int)> h = (int n) => bumpn(n);`) and with a non-void callee
(`Lambda<void(int)> f = (int x) => printf("x=%d\n", x);`) fail identically.

The BLOCK-body spelling `() => { bump(); }` compiles and runs correctly, so every void closure
in the repo today is written that way and the suites cannot see this.

## Root cause

`MainListener::ParseLambdaExpression` (`MainListener_PostfixExpression.cpp`, the lambda-body
arm) treats `=> expr` as `=> { return expr; }` unconditionally, so it emits a `ret <value>`
even when the lambda's declared return type is `void`. `CreateReturnCall` only emits
`CreateRetVoid` for a null value, and the value here is the callee's result (or a void-typed
call result), so a non-void `ret` lands in a void function.

## Fix direction

`=> expr` on a **void-returning** lambda is a DISCARDED full expression, not a return: evaluate
it as the expression-statement path does (`DiagnoseDiscardedOwningReturn` +
`RegisterDiscardedOwningStructTemp` + the block-item `FlushOwnedTemps`), then let the existing
`returnType.TypeName == "void"` arm a few lines below emit `CreateRetVoid`. Do NOT simply drop
the value on the floor - the discard path is where an owning result gets destructed, and a void
lambda body is exactly where an unclaimed owning temp would otherwise leak.

Accept set to freeze first: this newly ADMITS a construct, so per the standing rule enumerate
the ownership/destructor predicates a void lambda body will now reach.
