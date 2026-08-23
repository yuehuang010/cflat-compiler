# A unary operator overload never destructs its produced-temp receiver

Filed 2026-08-22 from the round-2 review of the ternary-receiver fix. Pre-existing: identical on
master before that fix.

## Repro

```
struct UniqueBox { unique Item* item; ~UniqueBox() { dtors++; } bool operator!() { return item == nullptr; } }
UniqueBox mk(int v);
bool a = !mk(9);                      // dtors stays 0 (1 due)
bool b = !(c ? mk(1) : mk(2));        // same
```

## Root cause

`TryUnaryOperatorOverload` (`MainListener_Expressions.cpp` ~6316) has no counterpart to the
receiver registration `TryBinaryOperatorOverload` (~6452) performs for produced temps (and for
ternary PHI arms), so the receiver temp is never entered in the owning-temp ledger.

## Fix direction

Mirror the binary-operator receiver registration (including the ternary-arm path and the
`receiverConsumes` polarity for a consuming `move` overload). Value + dtor-count legs in
`Test/test_move.cb` `testBucketAExpressionOwnership`; the `?:`-arm cell and the plain `mk()` cell
both fail pre-fix.
