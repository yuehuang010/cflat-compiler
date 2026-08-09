# A `return` whose value disagrees with the function's return type dies in the module verifier

Filed 2026-08-09 while fixing the void expression-body lambda verifier failure, which the same
commit fixes. Measured on that fix's merge base `4565f1e` (Release, macOS arm64, warm
`--init-local`) and unchanged by it - that fix deliberately scoped itself to the lambda
`=> expr` arm.

Severity: **P1 by the "no usable diagnostic" rule** - a raw LLVM verifier dump with no
`file(line,col):` prefix, reachable from two lines of ordinary source.

## Repro

Two families, both compile rc 1 with no binary and no source location.

Family A - a `return <expr>;` STATEMENT inside a void-returning function or lambda:

```cflat
int hits = 0;
void bump() { hits = hits + 1; }
void f() { return bump(); }        // "Found return instr that returns non-void ..."
void g() { return 5; }             // "... ret i8 5"
```

```cflat
Lambda<void()> g = () => { return bump(); };   // same dump
Lambda<void()> h = () => { return 5; };        // same dump
```

Family B - a value-less or void-typed `return` in a non-void function or lambda:

```cflat
void bump() { }
int f() { return; }                            // "Function return type does not match ..."
int g() { return bump(); }                     // same
Lambda<int()> k = () => bump();                // same
```

Family C - a BLOCK-body lambda literal that gets no expected return type (an immediately-invoked
literal), so the return type falls back to `void` and its `return <expr>;` lands in a void invoker:

```cflat
((int x) => { return x * 2; })(4)              // "Found return instr that returns non-void ..."
```

The EXPRESSION-body twin of Family C is diagnosed (`cannot infer the return type of lambda ...`);
the block body reaches `EmitReturnExpression` and is Family A again.

Sibling: consuming a void CLOSURE call's result reads garbage instead of failing the way the
direct `int r = bump();` spelling does - [[void-closure-call-result-consumed-reads-garbage]].

## Root cause

`MainListener::EmitReturnExpression` (`MainListener_Statements.cpp:432`) hands whatever
`ParseAssignmentExpressionNamed` produced to `CreateReturnCall` without comparing it against the
enclosing function's LLVM return type; `CreateReturnCall` emits `CreateRetVoid` only for a null
value. The value-less arm (`MainListener_Statements.cpp:1210`) is the mirror - it emits
`CreateRetVoid` with no check that the function returns void.

The lambda `=> expr` arm shared this root and was fixed in the filing commit, by routing a VOID
lambda's expression body through the expression-statement discard path rather than the return
lowering.
That fix deliberately did not touch the `return` STATEMENT, because the two positions want
different answers: `=> expr` is a discarded full expression, while `return 5;` in a void function
is a type error the user should be told about.

## Fix direction

Family A: in `EmitReturnExpression`, when the enclosing function returns void, `LogErrorContext`
naming the function and its declared `void` return type - unless the returned expression is itself
void-typed, in which case C semantics say evaluate it and emit `CreateRetVoid`. Family B: in the
value-less arm, `LogErrorContext` when the function's return type is not void; and reject a
void-typed value reaching a non-void return with the same message shape.

Because this ADMITS nothing and only converts verifier dumps into diagnostics, the accept set is
every currently-compiling `return`; freeze `Test/test_function_ptr.cb` and `Test/test_basic.cb`
before writing the guard. Note the guard must not fire on `return default;`
(`MainListener_Statements.cpp:1190`), which already special-cases a void return type.
