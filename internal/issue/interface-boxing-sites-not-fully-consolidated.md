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

## Two sharp edges - one CLOSED, one NARROWED

Both were inert (no reachable failure - the ledger's one consumer, `FatValueOwnsHeapBox`,
was gated behind `FrameLocalDataOfFatValue(right) == nullptr`, which rejected every
alloca-rooted data pointer before the lookup ran), but were closed as preventive hardening
so a future second consumer of the ledger does not inherit them silently:

- `ClassifyInterfaceBoxSource` (`MainListener.h`) now tests `isa<AllocaInst>` ->
  `FrameStorage` BEFORE the ownership test, so a by-value class local whose binding is
  `IsOwning` is classified `FrameStorage`, not `Heap`.
- `FindInterfaceBoxByDataPointer` (`LLVMBackend.h`) now takes an `InterfaceBoxSource`
  parameter and filters on it; the old unfiltered single-argument overload was deleted, so
  there is only one data-pointer lookup and a caller must say which kind of box it means.
  Its one caller, `FatValueOwnsHeapBox`, now passes `InterfaceBoxSource::Heap`. This
  NARROWS the edge rather than closing it - see the note below.

Verified behaviourally invisible: compared the fix binary against master on the ~67-program
corpus under `scratch/rev*` in the sibling `cflat-fix-return-dangle` worktree (interface
boxing/ownership/dangle shapes), plus `Test/test_interface.cb` and
`Test/errors/err_return_interface_value.cb` - identical exit codes and stdout in every case.

Note what is NOT fixed by this: `RegisterInterfaceBox` still dedupes on `FatValue` only, so
two records sharing BOTH a `DataPointer` and the same `Source` would still resolve to
whichever was registered first. The provenance filter narrows the sharp edge (a `Heap`
lookup can no longer see a `FrameStorage` record over the same pointer, and vice versa) but
does not remove the same-source collision case.

Be clear about which case that leaves: the motivating example - two interfaces boxed over
ONE object - lands on the SAME `Source`, so the filter does not discriminate it at all. It
is harmless today only because such a pair is either same-`Source` (both `Global`, both
`FrameStorage`) or impossible: a second box off an owning binding is a hard `use of moved
variable` error. Closing this properly means keying the dedupe on `(FatValue, DataPointer,
Source)` in `RegisterInterfaceBox`, not on `FatValue` alone.

## Fix direction

Route the two remaining open-coded sites (assignment-statement, call-argument) through
`BoxConcreteIntoInterface`, then restore the "this is the only place" claim in its comment.

## Related

[[interface-boxing-guards-are-binding-dependent]],
[[return-dangle-missed-when-slot-has-extra-user]], [[interface-issue-queue]]

The return-dangle issue this used to link (`interface-return-dangle-defeated-by-intermediate-local`)
was closed by `2bcc5a0`. Its residue above is the live successor, and consolidating the two
remaining open-coded boxing sites is the prerequisite for the provenance-based fix that
residue calls for.
