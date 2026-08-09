# `lambdaExpectedType` leaks into a nested lambda literal, so an inferred literal looks declared

Filed 2026-08-09 by the review of `fix/retvoid`, which does not fix it - the fix is in the
lambda CONTEXT threading, not in the return lowering that commit touches. Measured Release,
macOS arm64, warm `--init-local`, on the merge base `680696c` (`scratch/prefix_bin/cflat`) and
on the fix commit.

Severity: **P2**. The affected programs are still rejected, and with a real `file(line,col):`
diagnostic - only the WORDING is wrong, so it misdirects rather than leaving the user with
nothing.

## Root cause

`MainListener::ParseLambdaExpression` reads `lambdaExpectedType`
(`MainListener_PostfixExpression.cpp:4708`) to derive the literal's return type, but nothing
clears it before the body is walked (~`:4997`). A literal written INSIDE another lambda's body
therefore inherits the ENCLOSING lambda's expected return type, and `returnTypeInferred`
(`:4716`, "the expected return type is empty") is wrongly false. `lambdaExpectedType` is a
single listener-wide slot written by each context that threads a type in (declaration,
argument, field, `return`) and cleared by that context afterwards; there is no
save/clear/restore around the lambda BODY walk.

## Repro

### The pre-existing proof: inherited `int` on a VOID-bodied inner literal

`scratch/rev_34_iife_in_int_lambda.cb` - an immediately-invoked literal, which nothing supplies
a type for, written inside a DECLARED `Lambda<int()>` body:

```cflat
int hits = 0;
extern int main() { Lambda<int()> g = () => { (() => { hits = 1; })(); return 1; }; return g() == 1 && hits == 1 ? 0 : 1; }
```

**Identical on both binaries** (rc 1):

```
rev_34_iife_in_int_lambda.cb(2,47): Lambda '__lambda_1' missing return statement.
```

The inner literal is held to the OUTER lambda's `int`, so its void body reads as a missing
return. This is the cell that proves the leak predates `fix/retvoid`.

### The misdirected wording: inherited `void` on a value-returning inner literal

`scratch/rev_31_inferred_lam_in_declared.cb`:

```cflat
extern int main() { Lambda<void()> g = () => { (() => { return 7; })(); }; g(); return 0; }
```

| binary | result |
|---|---|
| `680696c` | rc 1, `Module verification failed:` - locationless dump |
| `fix/retvoid` | rc 1, `cannot return a value from a function whose return type is 'void' - drop the value ('return;'), or declare a non-void return type` |

So this cell is NOT identical pre/post - `fix/retvoid` correctly turned a dump into a
diagnostic. What the leak costs is that the diagnostic is the WRONG ONE: the same literal at
statement scope (`scratch/rv_c1_iife.cb`) gets `cannot infer the return type of lambda '...':
its body returns a value but no 'function<...>' or 'Lambda<...>' type reaches it here`, which
is the true story. The advice it gets instead is actively wrong - there is no `void` for the
user to "declare a non-void return type" instead of, because that `void` came from the
ENCLOSING lambda.

### Not every nested spelling is affected

`scratch/rev_35_iife_value_in_int_lambda.cb` - a VALUE-returning inner literal inside a
declared `Lambda<int()>` - **compiles and runs rc 0 on BOTH binaries**:

```cflat
int hits = 0;
extern int main() { Lambda<int()> g = () => { hits = (() => { return 7; })(); return hits; }; return g() == 7 ? 0 : 1; }
```

The inherited `int` happens to be the right answer there, so the leak is invisible. Any fix
must keep this cell compiling and returning 0.

The nesting direction that is already correct is the one where the OUTER literal is the
inferred one (`scratch/rv_n3_restore_after_inner.cb`, `scratch/rv_n4_declared_inner_in_inferred.cb`):
the `fix/retvoid` RAII scope keys on the lambda's own invoker, so an inner lambda's return is
judged against the inner invoker. Only OUTER-declared / INNER-inferred is wrong, and it is
wrong before the return lowering is ever reached.

## Fix direction

Give `ParseLambdaExpression` the same RAII treatment the inferred-return state already has:
after reading `lambdaExpectedType` into `returnType` (`:4708-4718`), save it, CLEAR it, and
restore on scope exit, so a nested literal sees an empty slot unless its own context supplied
one. `LogErrorContext` throws, so this must be RAII, not a manual restore at the end.

Accept set to freeze first: `rev_35` above, every legal nested-lambda spelling in
`Test/test_function_ptr.cb`, and `return () => {...};` from a `Lambda<>`-returning function
(threaded at `MainListener_Statements.cpp:460`). The shape most likely to regress is a literal
passed as an ARGUMENT from inside another literal's body - the argument context writes the slot
immediately before the literal is parsed, so the clear must not race it.
