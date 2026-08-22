# An owning-struct temp produced in a ternary argument is never destructed

Filed 2026-08-22 from the round-3 review of the alias-parameter fix (40b1e17). Reproduces
identically on master 6f38504 before that change, for both the by-value and the `alias`
parameter spelling, on either arm.

## Repro

```
struct A { int* p; A(int v) { p = new int; *p = v; } ~A() { delete p; dtors++; } }
A mk(int v) { return A(v); }
void takeV(A a) { }
void takeA(alias A a) { }
int main() {
    A named(1);
    bool h = argc > 5;
    takeV(h ? named : mk(8));   // dtors counts 1 where 2 are due; HeapAudit reports the leak
    takeA(h ? named : mk(8));   // same
}
```

## Root cause (hypothesis)

The ternary join merges a named-local arm (handover, must NOT be destructed by the call) with
a call-result arm (temp, must be destructed once); the merged value carries the handover
identity so the temp arm is never registered in the owning-temp ledger. Adjacent to
[[mixed-owning-borrow-struct-ternary-join-leaks-the-owning-arm]], which is the same join with an owning/borrow
mix instead of an owning/temp mix.

## Fix direction

In `ParseConditionalExpression`, when one arm is a produced temp (`IsProducedTempValue`) and
the other is storage-backed, emit the temp arm's destructor on a per-arm flag (or copy the
named arm into a fresh temp so both arms are temps), the way the `?.` ledger does for null
chains.
