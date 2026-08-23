# `a *= b` on a struct with `operator*` emits a raw struct `mul` and dies in the LLVM verifier

Filed 2026-08-22 from the round-2 review of the ternary-receiver fix. Pre-existing on master.

## Repro

```
struct EBox { int v; EBox operator*(EBox o) { ... } }
EBox acc = mk(2);
acc *= mk(3);     // Module verification failed: %3 = mul %EBox %2, %1
```

## Root cause

Compound assignment does not route through `TryBinaryOperatorOverload`; it lowers the operator
on the LLVM struct type directly, which only the verifier rejects.

## Fix direction

Route `op=` through the overload lookup for the matching binary operator (`a = a op b` semantics,
with the receiver registration the binary path already does), or emit a source-level `LogError`
("no operator*= overload for 'EBox'") when none matches - a verifier dump is never the right
diagnostic (CLAUDE.md rule). Legs: a value leg for `*=` via `operator*` and an expect_error leg
for a struct with no overload, in existing operator tests.
