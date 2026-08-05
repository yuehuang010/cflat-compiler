# `move` of a BORROWED pointer adopts into a plain `T*` destination (silent double free)

Filed 2026-08-05 by `fix/ptrcopy`, which closed the sibling shapes and deliberately did NOT close
this one. Measured identical on `d93c359` and on the merged `fix/ptrcopy`: exit 134, no diagnostic.

## Root cause

`move` of a source that carries `IsBorrowed` is rejected only when the DESTINATION is `unique`
(`MainListener.h`'s `srcIsBorrowed && ... typeAndValue.IsUnique` gate at the declaration, and its
`RejectBorrowIntoUniqueLocal` twin on the `=` path). A plain `T*` destination has no such gate, so
the destination adopts ownership the borrow never had and the real owner frees the pointee again.

`fix/ptrcopy` added a destination-agnostic move guard for the two proofs its issue named -
`OwningLocalCopyStillAliases` (a copy of a live owning local) and `BorrowsOwnedElement` (a
container-owned element). `IsBorrowed` was deliberately left out of that guard, because
`MainListener.h` states an explicit, ratified policy directly above the site:

> Forwarding an ordinary borrow as 'move' stays legal (the programmer asserts the borrow is dead)

That policy is what makes this a separate decision rather than a missed case. The measurements
below are the evidence that the policy is unsound for a POINTER parameter, where the callee cannot
know whether the caller's borrow is dead.

## Repros

Common prelude:

```cflat
int dtorCount = 0;
class Ci { int r = 7; int area() { return r; } ~Ci() { dtorCount = dtorCount + 1; } };
```

### (a) `move` of a borrowed PARAMETER directly - exit 134 on both binaries

```cflat
int paramMove(Ci* p) { Ci* d = move p; return d->area(); }   // `d` adopts; the caller still frees
// caller: Ci* c = new Ci(); paramMove(c); delete c;
```

### (b) `move` of a COPY of a borrowed parameter - exit 134 on both binaries

```cflat
int paramCopyMove(Ci* p) { Ci* b = p; Ci* d = move b; return d->area(); }
```

`b` carries `IsBorrowed` (propagated at its declaration by the existing `srcIsBorrowed` clause), so
this is the same defect one hop out, not a separate propagation gap.

### (c) The same shape in a `move`-RETURNING function - exit 134 on both binaries

```cflat
move Ci* handOff(Ci* own) { Ci* b = own; return move b; }
// caller: Ci* c = new Ci(); Ci* r = handOff(c); delete r;   // and `c` frees again
```

## Fix direction

Add `IsBorrowed && !BorrowedOrigin.empty()` as a third proof to the destination-agnostic move guard
`fix/ptrcopy` added in `ParseMoveExpression` (the two existing proofs are right beside it, and the
raw-`delete` guard already rejects exactly this set - `cannot delete borrowed parameter 'p'` and
`cannot delete 'b' - it aliases borrowed parameter 'p'`, so this closes the same delete/move
disagreement the copy members closed).

**Reopening the quoted policy is the actual work here, not the guard.** Build the accept set first:
the policy exists because some `move` of a borrow is a deliberate forward, and the whole point of
`move` on a by-value owning-value parameter is already handled separately (the
`IsVariableBorrowedOwningValue` degrade-to-read branch immediately below the site). Enumerate at
minimum: a `move` param forwarded onward as `move`, a borrow forwarded into a sink parameter, a
borrow forwarded through a `move`-returning wrapper whose caller does NOT free, and the
`alias`-return spellings - each measured on the current binary BEFORE any guard is written.

## Related

[[interface-issue-queue]] - the `fix/ptrcopy` landed design record states why this member was
scoped out, and holds the accept set the move guard was built against.
