# Forwarded closures: remaining refusals after FNWRAP

T525 fixed the wrong-code cells recorded here. FNWRAP also fixed the located refusals for a
function-pointer lvalue-reference result, by-value closure owners, closure moves after a holder
expires or on a disjoint branch, a `F&&` result that is the closure itself (`pass(f)`), and a
by-value closure result (`copy_return(f)`). Generated wrapper `closureKeptParams` metadata identifies
which arguments are retained, and live holder variables carry that evidence until their scope ends.

## 1. A moved or temporary closure into a result that keeps a reference

```cflat
std.cflat_t28.call5(std.cflat_t28.fwd(move f));
t28.holdptr(move f);
```

Still refused with `caller-owned C++ closure slot`. CFlat `move f` drains `f` into a temporary that
dies at the end of the statement, so a kept reference would dangle. Matching clang would need a
caller-frame temporary destroyed at the end of the full expression plus a rule for what `move` means
when the source stays alive. A lambda literal argument has no typed context and is refused earlier
(return type inference), so it never reaches this check.

## 2. Moving a closure while a live C++ result keeps its reference

```cflat
auto h = t28.holdfwd(f);
Lambda<int(int)> g = move f;
```

Still refused: `h` may call `f`, and `move` leaves `f` null. The wrapper's harvested
`closureKeptParams` fact is attached only when the result is a keeper, then checked against live
holder variables. An opaque callee with no visible body, virtual dispatch, function-pointer call,
or analysis that reaches the depth limit remains conservatively treated as a keeper. The declaration-
only `t525.holdopaque` case therefore remains refused.

A by-value owner such as `OwnedBox<F>` is accepted. A holder in a scope that has ended, or a holder
created in a disjoint branch, no longer blocks `move f`.

Found by: t28 sibling audit (2026-10-03); T56, T69, T419, T525 rounds; FNWRAP (2026-11-05).
