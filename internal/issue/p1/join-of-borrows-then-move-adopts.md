# `move` of a local bound from a JOIN of two borrows adopts and double-frees

P2, PRE-EXISTING. Measured identical on `f1b8116` and on the `fix/move-borrowed-plain-dest` branch head: rc 133, no diagnostic. Filed 2026-08-08 by `fix/move-borrowed-plain-dest`.

## What

`T* j = k > 0 ? p : q;` where both arms are borrowed parameters records `JoinKeepsOwner` on `j`,
and the raw `delete j;` is correctly rejected with
`cannot delete 'j' - every arm of the join it was bound from holds an object 'p' already frees`.
`move j` consults neither `JoinKeepsOwner` nor `IsBorrowed` (a join carries no source binding, so
the declaration's borrow clause never fires), so the destination adopts and frees a pointee both
callers still own.

## Repro (rc 133 on both binaries)

```cflat
int dtorCount = 0;
class Ci { int r = 7; int area() { return r; } ~Ci() { dtorCount = dtorCount + 1; } };
int f(Ci* p, Ci* q, int k) { Ci* j = k > 0 ? p : q; Ci* d = move j; return d->area(); }
extern int main() { Ci* a = new Ci(); Ci* c = new Ci(); int v = f(a,c,1); delete a; delete c; return 0; }
```

The no-`move` twin (`Ci* j = k > 0 ? p : q; delete j;`) is REJECTED, so this is the same
delete/move disagreement the copy members closed, on the join axis.

## Root cause

`fix/ptrcopy` set `JoinKeepsOwner`'s consumers as raw `delete`, boxed `delete` and the unique-field
store - deliberately not `move` (see its landed record, member (d)). `fix/move-borrowed-plain-dest`
made a plain destination decline to adopt a `move` of an `IsBorrowed` source, but a join result
carries no `IsBorrowed`, so it is not reached.

## Fix direction

Ask `JoinArmsStillKeepOwner` in `ParseMoveExpression` alongside the existing two proofs, or - to
match the plain-destination shape of the sibling fix rather than adding a rejection - carry the
join proof into the move RESULT so the destination declines to adopt. The BOTH-ARMS rule and its
`JoinArmsStillKeepOwner` retirement already exist and should be reused unchanged; a MIXED join must
stay accepted, which is what keeps this out of the false-rejection direction.
