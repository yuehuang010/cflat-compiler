# An owning-struct temp produced by a ternary leaks when used as a method RECEIVER or a brace-init FIELD

Filed 2026-08-22 from the round-2 review of the ternary-call-argument fix (bucket A). Pre-existing:
identical on master before that fix. Not a regression of it.

## Repro

```
struct UniqueBox { unique Item* item; ... ~UniqueBox() { dtors++; } }
UniqueBox mk(int v);
struct Wrapper { UniqueBox b; }
void takeWrapper(Wrapper w);

int a = (c ? mk(1) : mk(2)).boxId();          // dtors stays 0 (1 due)
int b = (c ? mk(1) : mk(2)).item->idOf();     // same
takeWrapper({ b = c ? mk(1) : mk(2) });       // dtors stays 0 (1 due)
```

## Root cause (hypothesis)

The ternary-temp ledger fix registers a produced ternary temp only while `inCallArgument_` is set
(the ternary is directly a call argument, at any nesting depth of ternaries). A ternary used as a
method-call receiver, or as a field initializer inside a brace-init that is itself an argument, is
not in that set, so the merged PHI value is never registered in the owning-temp ledger.

## Fix direction

Extend the registration to the receiver position (`ParsePostfixExpression` receiver evaluation)
and to brace-init field values, reusing the same PHI identity handling. Value+dtor-count legs in
`Test/test_move.cb`, `testBucketAExpressionOwnership`.
