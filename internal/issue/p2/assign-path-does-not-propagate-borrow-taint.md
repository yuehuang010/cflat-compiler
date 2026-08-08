# A plain `=` does not propagate the borrow taint, so `delete` through the copy is accepted

P2, PRE-EXISTING. Measured identical on `f1b8116` and on the `fix/move-borrowed-plain-dest` branch head: rc 133, one `dtor` printed then abort, no diagnostic. Filed 2026-08-08 by
`fix/move-borrowed-plain-dest` while scoping the `move` spelling of the same hole.

## What

The DECLARATION path propagates `IsBorrowed` from a borrowed source to the new local
(`MainListener_Declarations.cpp`, the `srcIsBorrowed` clause), which is what lets the raw-`delete`
guard reject `T* d = p; delete d;`. The plain `=` store path has no counterpart: it sets
`PointerRebound` and clears the declaration-time facts, but never RECORDS that the RHS was itself a
borrow. A local that becomes a borrow by assignment is therefore untracked.

## Repro (rc 133 on both binaries)

```cflat
int dtorCount = 0;
class Ci { int r = 7; ~Ci() { dtorCount = dtorCount + 1; } };
void f(Ci* p) { Ci* d = nullptr; d = p; delete d; }
extern int main() { Ci* c = new Ci(); f(c); printf("dtor=%d\n", dtorCount); return 0; }
```

No `move` is involved. The declaration spelling of the same program
(`void f(Ci* p) { Ci* d = p; delete d; }`) IS rejected -
`cannot delete 'd' - it aliases borrowed parameter 'p'`.

The `move` spellings ride on the same gap and are NOT separate bugs:
`Ci* d = nullptr; d = move p; delete d;` and the `??=` form both abort the same way.

## Root cause

`ParseAssignmentExpression`'s pointer store tail (`MainListener_Expressions.cpp`, the
`MarkPointerRebound` block) retires declaration-time facts and re-arms only the OWNER-side proofs
(`InheritedKeepsOwner`, `JoinKeepsOwner`, `BorrowsOwnedElement`). `IsBorrowed` / `BorrowedOrigin`
are neither retired nor re-established there.

## Fix direction

Record the borrow on the `=` exactly as the declaration does: when the RHS binding carries
`IsBorrowed` (and the store is not a proven ownership transfer - `srcIsOwnedPtrRhs` is the
discriminator already computed at that site), set `IsBorrowed` / `BorrowedOrigin` on the LHS
binding rather than leaving it blank. Care is needed with the flow-insensitivity hazard tracked in
[[conditional-store-retires-borrow-facts-unconditionally]]: RECORDING a borrow is the safe
direction, so a conditional store may over-record and reject, which is preferable to laundering.

## Related

[[interface-issue-queue]] - `fix/move-borrowed-plain-dest`'s landed record scopes this out
explicitly and names it as the reason the `=` and `??=` `move` spellings were not closed there.
