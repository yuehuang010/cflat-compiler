# Two class-to-interface boxing sites are still open-coded

Filed 2026-07-29 by the review of the boxing consolidation.

Severity: NO behavioural defect today - the review probed specifically for an ownership
asymmetry between the routed and un-routed sites and found none. This is consolidation
debt: the invariant that made the original bug family possible is still not enforced.

## What was consolidated and what was not

`BoxConcreteIntoInterface` (`MainListener.h:9969`) is now the shared boxing site for two
of the four paths - the declaration-initializer path and `GenerateSafeCast` (the `as`
path). Its comment states this plainly and names what remains.

Still open-coded, each repeating implements-check -> `RejectPointerShapedInterfaceUpcast`
-> data-pointer selection -> `BuildInterfaceFatValue`:

- `MainListener.h:10672-10690` - the assignment-STATEMENT path (`IShape s; s = c;`), which
  is a different code path from the declaration-initializer (`IShape s = c;`) that was
  routed.
- `MainListener.h:14642` - the call-argument boxing path.

## Why it matters despite no live defect

The entire `as`-boxing bug family existed because four copies of this bookkeeping drifted
apart, each carrying a different subset of the guards. Two copies remain. The next guard
added to the helper will silently not apply to them, which is exactly how the family
started.

Verified today: the assignment-statement leg's ownership bookkeeping lives elsewhere and
happens to agree with the helper's, including the use-of-moved diagnostic.

## Two sharp edges in the new code, both inert today

Recorded here so they are not lost - neither has a reachable failure:

- `ClassifyInterfaceBoxSource` (`MainListener.h:9957-9979`) tests
  `ownershipTransferred || srcNV->IsOwning || IsOwningValue(dataPtr)` -> `Heap` BEFORE it
  tests `isa<AllocaInst>` -> `FrameStorage`, so a by-value class local whose binding is
  `IsOwning` would be classified `Heap` with an alloca data pointer.
- `RegisterInterfaceBox` dedupes on `FatValue` only, so two records can share a
  `DataPointer` and `FindInterfaceBoxByDataPointer` returns the first.

Both are unreachable because the ledger has exactly ONE consumer today
(`FatValueOwnsHeapBox`, `MainListener.h:5570`) and that call site is gated behind
`FrameLocalDataOfFatValue(right) == nullptr`, which rejects every alloca-rooted data
pointer before the lookup runs.

**This matters for whoever closes
[[interface-return-dangle-defeated-by-intermediate-local]]**, because that change adds the
SECOND consumer of the ledger and it will not be behind that gate. Fix the classifier
ordering as part of that work, or prove the new consumer is insensitive to it.

## Fix direction

Route the two remaining sites through `BoxConcreteIntoInterface`, then restore the
"this is the only place" claim in its comment. Reorder `ClassifyInterfaceBoxSource` so the
storage-shape test precedes the ownership test.

## Related

[[interface-boxing-guards-are-binding-dependent]],
[[interface-return-dangle-defeated-by-intermediate-local]], [[interface-issue-queue]]
