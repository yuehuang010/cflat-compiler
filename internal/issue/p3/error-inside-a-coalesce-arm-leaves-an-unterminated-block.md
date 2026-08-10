# An error thrown inside a `??` arm leaves an unterminated block

Filed 2026-08-10 during the `fix/coalarm` review. PRE-EXISTING: identical on `0535f48` and on
`fix/coalarm`. P3: the `expect_error` still reports PASS and the exit code is still 0, so no
test result is wrong - but the module does not verify and the diagnostic noise is misleading.

## What

`ParseTernaryBranches` wraps its two arms in a `try` / `catch (...)` that terminates every
half-emitted arm block with a branch to the resume block and then calls `DiscardOwnedTempsSince`,
precisely so an error thrown mid-arm leaves a module that still verifies. The `??` lowering in
`ParseConditionalExpression` (`cflat/MainListener_Expressions.cpp`) has no equivalent: an error
raised while lowering the right operand escapes with `nullcoal_null` unterminated, and the owned
temps that arm already registered stay in the ledger keyed to a block that no longer reaches the
join.

## Repro (`scratch/rev_e1.cb`)

```cflat
class Res { int v = 0; ~Res() { } };
extern int main(){
    Res* n = nullptr;
    expect_error("Undefined variable nosuch") {
        Res* k = n ?? nosuch;
    }
    return 0;
}
```

```
rev_e1.cb(5,22): Undefined variable nosuch.
PASS: expected error received
Module verification failed:
Basic Block in function 'main' does not have terminator!
```

## Fix direction

Mirror `ParseTernaryBranches`: wrap the `??` arm lowering in the same `try` / `catch (...)` that
terminates `nullcoal_null` (and `nullcoal_notnull`, if it is still open) with a branch to
`nullcoal_resume`, switches the insert point to the resume block, calls
`DiscardOwnedTempsSince(rhsMark)`, and rethrows.
