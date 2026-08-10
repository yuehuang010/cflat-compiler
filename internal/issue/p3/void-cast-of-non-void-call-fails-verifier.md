# `(void)take(5);` on a NON-void call dies in the module verifier

Filed 2026-08-09 from the fix/dvoid review. Pre-existing: byte-identical on base `43ccb90` and on
the fix branch. It is the sibling of the `(void)` cast line fix/dvoid touched (the cast now parses
its OPERAND as Discard, which fixed `return (void)f();` for void f) - but casting a non-void
result to void was broken before and after.

Severity: locationless verifier failure reachable from plain source (the standing rule would make
this P1 on reachability; filed P3 because the spelling is rare in C-style code - the idiomatic
discard is a bare statement call, which works).

## Repro

```cflat
int take(int x) { return x; }
extern int main() {
    (void)take(5);   // Module verification failed: Invalid bitcast / bitcast i32 %0 to void
    return 0;
}
```

rc 1, no source location, on both binaries. The bare `take(5);` statement is rc 0.

## Root cause

The cast path emits an actual bitcast of the i32 result to LLVM void instead of just dropping the
value. A cast TO void should discard: evaluate the operand for effects and produce no value.

## Fix direction

In the cast arm (MainListener_Expressions.cpp ~5087, where the void-cast Discard threading landed),
when the destination type resolves to void and the operand produced a real value, drop the value
instead of emitting any conversion. Keep the operand's side effects. The existing accept legs
(`(void)dvaBump();`, `return (void)f();`) must stay green.
