# unique-field-to-field-interface-receiver-residues

## 2026-08-10 - SUPERSEDED by the ruling on [[unique-field-to-field-array-element-receiver]]

The maintainer ruled that a plain `=` between two `unique` fields is a **uniform implicit move** -
source nulled, old destination released, no proof required and no rejection. Every shape in this
file is a MISSING DIAGNOSTIC caused by an unprovable receiver pair, so all five stop needing a proof
and stop double-freeing. The five-shape inventory below is retained as the record of what the
prove-then-reject design could not reach; it is no longer work to be done.

`SoleStoreIntoSlot` and the interface half of `ProvablyDifferentObjects` become deletable with the
rest of that machinery.

**One item here does NOT close with the ruling** and should be re-filed separately if it matters:
shape 5's underlying cause - the four deferred end-of-body checks (`RunUniqueIfaceFieldStoreCheck`,
`RunNullDerefDataflow`, `RunInterfaceReturnDangleCheck`, `RunNullIfaceDispatchCheck`) never run for
LAMBDA bodies, only for the named-function path. That is a pre-existing architectural gap affecting
three checks that this ruling does not touch, and one shared hook fixes it for all of them.

**Severity: P3 (deliberate deferral).** Filed 2026-08-05 out of the round-1 review of
`fix/iface-selfassign` (the `interface-field-self-assign-false-positive` fix). Every shape here
is a MISSING diagnostic, never a false rejection: the program compiles clean on both the pre-fix
and post-fix binaries and double-frees at runtime (rc 134 under the default allocator). These are
the receiver shapes the landed proof deliberately cannot reach; the P3 ranking follows the
`return-dangle-missed-when-slot-has-extra-user` precedent that a permanent-or-deferred non-fix
does not belong in the P1 working set. Re-rank if the maintainer rules otherwise.

## The five residue shapes

The landed check proves two interface receivers wrap DIFFERENT objects by resolving each
receiver's fat pointer back through its slot's sole box store to the boxed object's alloca or
global root (`SoleStoreIntoSlot` + `ProvablyDifferentObjects`). Each shape below breaks one link
of that chain, so the pair stays unprovable and the store is accepted:

1. **Pointer-boxed receiver** - `IFace i = *p;` boxes through a load, not an alloca root.
2. **`new`-boxed receiver** - the data root is a heap call; `ProvablyDifferentObjects` only
   speaks about distinct `AllocaInst` / `GlobalVariable` roots.
3. **Interface PARAMETER or call-RESULT receiver** - the slot has no box store in this body at
   all, so `SoleStoreIntoSlot` has nothing to resolve.
4. **Two sub-objects of one container** (two elements of one array, two fields of one struct) -
   a single root; distinct-root proof cannot fire, and runtime indices make offset proofs
   unsound (see [[unique-field-to-field-array-element-receiver]], the sibling residue for the
   plain struct mechanism).
5. **Flagged store inside a LAMBDA body** - the end-of-body hook that settles the deferred
   records runs only in the named-function path (`MainListener.h`, the
   `RunUniqueIfaceFieldStoreCheck` call site); lambda bodies run none of the four deferred
   checks. The record is keyed by the lambda's own `llvm::Function`, so nothing leaks into the
   enclosing function - the diagnostic is simply never rendered. This is a pre-existing
   architectural gap shared with `RunNullDerefDataflow`, `RunInterfaceReturnDangleCheck`, and
   `RunNullIfaceDispatchCheck`.

Also unprovable but note-only: a GLOBAL interface local (slot is a `GlobalVariable` whose stores
span functions).

## Repro (shape 1, verified on the fixed binary; the others differ only in how the receiver is bound)

Same declarations as `Test/errors/err_unique_field_to_field.cb`:

```cflat
struct Node { int v = 0; };
interface ISlotIf { unique Node* slot; };
class BoxIf : ISlotIf { unique Node* slot = nullptr; };

void stealThroughPointerBox()
{
    BoxIf a = default;
    BoxIf c = default;
    a.slot = new Node();
    BoxIf* pa = &a;
    ISlotIf ia = *pa;      // boxed through a pointer load - the data root is unprovable
    ISlotIf ic = c;
    ic.slot = ia.slot;     // accepted; two owners of one Node -> double free (rc 134)
}
```

## Fix direction

Extending the proof needs value identity that survives loads and calls - the same ledger the
open P1 `join-erases-code-value-evidence-at-every-gate` asks for on the code-value side. Points
worth taking in one future change: give `ProvablyDifferentObjects` a heap-call arm (two distinct
`new` results are distinct objects), teach the box resolution to look through a single
provably-unaliased pointer load, and run the four deferred end-of-body checks for lambda bodies
(one shared hook fixes shape 5 for all four checks at once). Polarity stays prove-then-reject:
anything still unprovable keeps compiling.

Related: [[unique-field-to-field-array-element-receiver]], [[interface-issue-queue]]
