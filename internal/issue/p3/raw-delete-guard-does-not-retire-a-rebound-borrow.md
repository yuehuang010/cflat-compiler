# The raw-`delete` guard never retires a borrow, so a rebound copy is falsely rejected

P3 false rejection, PRE-EXISTING. Measured identical on `f1b8116` and on
the `fix/move-borrowed-plain-dest` branch head. Filed 2026-08-08 by `fix/move-borrowed-plain-dest`.

## What

The raw-`delete` guard's borrow legs test `IsBorrowed && !BorrowedOrigin.empty()` (and, for a
parameter, `IsFunctionParameter`) with NO retirement test at all. A binding that was pointed at a
fresh owner is then still blamed on the parameter it used to alias, and the message's remedy
("Declare the source parameter 'move raw'") does not describe the program.

## Repro (hard error on both binaries; the program is correct and frees once)

```cflat
struct Payload { int value = 0; };
void f(Payload* raw) { Payload* b = raw; b = new Payload(); delete b; }   // rejected
void g(Payload* raw) { raw = new Payload(); delete raw; }                 // rejected
```

`fix/move-borrowed-plain-dest` gave the `move` spelling of the same programs a retirement test
(`BorrowProofRetiredByRebind`: the RHS was a provably owned value AND the store is in the same
basic block), so `move`+`delete` now accepts what plain `delete` still rejects.

## Why it was not fixed there

It is the SAFE direction (it rejects correct programs rather than laundering broken ones), and
widening a delete guard is a change with its own accept set. It also blocks the obvious shortcut in
the sibling area: `IsOwning` alone cannot serve as the "now holds an owner" discriminator, because
the plain `=` path never sets it - which is why `ReboundToOwnedValue` had to be recorded from
`srcIsOwnedPtrRhs` instead.

## Fix direction

Reuse `LLVMBackend::BorrowProofRetiredByRebind` at the two borrow legs of the raw-`delete` guard,
and at the boxed-`delete` twin. The same-basic-block half of that predicate is what keeps a
never-taken `if (b == nullptr) { b = new T(); }` from laundering the delete - see
[[conditional-store-retires-borrow-facts-unconditionally]].
