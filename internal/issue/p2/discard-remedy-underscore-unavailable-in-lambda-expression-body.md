# The discarded-owning-return remedy `_ =` cannot be written in a lambda expression body

Filed 2026-08-09 during the review of the void expression-body lambda fix, which made
`DiagnoseDiscardedOwningReturn` newly reachable from a VOID lambda's `=> expr` body.

Severity: **P2** - the rejection itself is correct and two of the message's remedies do work, but
one named remedy is a closed loop at this position.

## Repro

```cflat
class SBox { string s = default; ~SBox() { } };
SBox makeS() { SBox b = default; b.s = "abc"; return b; }
extern int main()
{
    Lambda<void()> g = () => makeS();
    g();
    return 0;
}
```

```
(5,29): owning return value of 'makeS' must not be discarded; bind it, move it, delete it,
        pass it on, or discard it explicitly with '_ ='
```

Applying the named remedy at that position does not work:

| spelling | result |
|---|---|
| `() => _ = makeS()` | `Undefined variable _.` |
| `() => (_ = makeS())` | the SAME "must not be discarded" error - a closed loop |
| `() => delete makePtr()` | works |
| `() => sink(makeS())` | works |
| `() => { _ = makeS(); }` (block body) | works |
| `_ = makeS();` (statement) | works |

## Root cause

`_ = expr` is handled in `ParseAssignmentExpression`
(`MainListener_Expressions.cpp:887`). The statement position reaches that arm; the lambda
expression body goes through `ParseAssignmentExpressionNamed`, whose fast path resolves `_` as an
ordinary identifier and fails. Both spellings behave identically on the pre-fix binary, so this is
a pre-existing gap in `_` handling, not a regression - what changed is that a message naming `_ =`
is now emitted at a position where `_ =` cannot be written.

The parenthesized form fails for a second reason: the outer expression has no
`assignmentOperator()`, so the discard arm's `bareExpr` is true and it re-diagnoses the inner
explicit discard. The expression-STATEMENT path has exactly the same behaviour for
`(_ = makeS());`, so the lambda arm is faithfully mirroring it.

## Fix direction

Make `ParseAssignmentExpressionNamed` route a top-level `_ =` to the existing discard arm, so the
remedy is writable everywhere the message is emitted; and teach `bareExpr` to see through a
parenthesized top-level assignment so the explicit discard is not re-diagnosed (fixes the
statement position too). Freeze the working remedies in the table above as value legs first - the
standing rule is that a message's remedy is a factual claim and must compile per position.
