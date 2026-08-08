# Copying an owning struct out of a deref/field/element source into an owning local double-frees

Filed 2026-08-07 by round-1 review of `fix/aliaslaunder`, which probed these shapes while checking
the new borrow-adoption guard's storage-shape gate and found they reduce to a BROADER pre-existing
bug with no alias involved anywhere. Identical on `86f929b` and on the merged `fix/aliaslaunder`
(residue, not regression). Not covered by `alias-borrow-remaining-launder-sites.md`, whose three
cells all start from an `IsAliasBorrow` binding.

## Repros (all: compile 0, run rc 133 silent double free; prelude as in the alias family -
`Res` with dtor counter, `Box { unique Res* item; }`, `makeBox`)

```cflat
// (a) through a pointer - NO alias anywhere
Box a = makeBox(5); Box* ap = &a;
Box other = makeBox(1);
other = *ap;            // rc 133 on both binaries

// (b) plain field of an owner
Wrap w; w.b = makeBox(5);
Box o = makeBox(1);
o = w.b;                // rc 133 on both binaries

// (c) array element source - same shape, measured rc 133 on both
```

## Root cause (hypothesis, from the guard work next door - verify)

The owning-value reassignment path in `ParseAssignmentExpression` handles a SLOT-backed source
(alloca/global with a `CallerName`) by classifying via `ClassifyOwningAssignSource` and
transferring; an indirect source (deref, field access, element) misses that gate and falls to a
bits-copy that never nulls the true source, so both the destination and the original owner
destruct the same `unique` pointee. The alias family fixed by `fix/aliaslaunder` was a special
case of this reachable through `IsAliasBorrow`; this file is the general case.

## Fix direction

Decide per sub-case whether the correct answer is transfer (null the indirect source's slot -
needs a writable lvalue path) or reject with a "use '.copy()'" diagnostic like the borrow
adoption one. The accept set to freeze first: `.copy()` spellings, POD-struct sources (no
`unique` field - must stay a plain copy), and slot-backed `b = a` transfer behaviour.

## Related

[[alias-borrow-remaining-launder-sites]], [[alias-borrow-local-launder-gaps]] (deleted, see the
`fix/aliaslaunder` landed record in [[interface-issue-queue]]).
