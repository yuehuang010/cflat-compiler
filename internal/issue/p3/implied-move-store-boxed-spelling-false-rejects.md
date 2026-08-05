# The BOXED spelling of an implied-move store is false-rejected (the raw one is correct)

Filed 2026-08-05 by `fix/ptrcopy`, which nearly reproduced this defect in the raw-`delete` guard and
caught it against its own accept set. PRE-EXISTING: measured identical on `d93c359` and on the
merged `fix/ptrcopy` - this is not a regression of that change, and closing it is not a prerequisite
for anything that change landed.

## Root cause

A plain `p = c;` store between two pointer LOCALS is an IMPLIED MOVE: `TransferPointerOwnershipOnStore`
nulls `c`, so `p` becomes the sole owner and `delete p;` is correct (measured: one free, exit 0).

But `MarkPointerRebound` runs BEFORE that transfer, and it asks `DescribeAssignedSourceOwner(rightNV)`,
which answers `'c'` because `c` is still `IsOwning` at that moment. The store therefore sets
`InheritedKeepsOwner` on `p` - a fact that is already stale by the end of the same statement.

Nothing reads it in the raw-`delete` guard, so the raw spelling is unaffected. But
`BindingKeepsOwnershipOfBoxedObject` DOES read it (`MainListener.h`, above the `PointerRebound`
retirement), so the boxed spelling is rejected with a message that is false at that site.

## Repro

```cflat
int dtorCount = 0;
interface IS { int area(); };
class Ci : IS { int r = 7; int area() { return r; } ~Ci() { dtorCount = dtorCount + 1; } };

Ci* c = new Ci();
Ci* p = nullptr;
p = c;             // implied move: `c` is nulled here
delete p;          // CORRECT - one free, exit 0. Accepted on both binaries.

// ...but the boxed twin of the same store:
IS s = p;
delete s;          // REJECTED on both binaries: "cannot delete interface 's' - it boxes an
                   // object that 'c' already frees" - `c` is null by then and frees nothing.
```

Both halves measured on `d93c359` and on the merged `fix/ptrcopy`; the raw half is `V=1 D=1`, exit 0,
and the boxed half is a hard error on both.

## Why P3

It is a false rejection with a working remedy (delete the raw pointer instead of boxing it), the
message is merely mis-blamed rather than dangerous, and the shape is narrow. It is recorded because
the NEXT person to touch `InheritedKeepsOwner` needs to know the field carries a stale answer for
this one store shape - `fix/ptrcopy` introduced `JoinKeepsOwner` as a SEPARATE field for exactly
this reason, and would have shipped a false rejection of the raw spelling had it reused
`InheritedKeepsOwner` (its `assign_copy_delete_*` legs in `Test/test_move.cb` are that tripwire).

## Fix direction

Ask `DescribeAssignedSourceOwner` AFTER the ownership transfer, or have it answer empty when the
store is one `TransferPointerOwnershipOnStore` will consume - the same condition that site already
computes. Do not simply drop the `InheritedKeepsOwner` clause from the boxing proof: it is what
stops `p = q;` between two borrowed PARAMETERS from laundering a borrow (pinned by the
`sPropagated` leg in `Test/errors/err_delete_borrowed_interface_box.cb`).

## Related

[[interface-issue-queue]] - the `fix/ptrcopy` landed design record explains why the join proof is a
separate field from `InheritedKeepsOwner`.
