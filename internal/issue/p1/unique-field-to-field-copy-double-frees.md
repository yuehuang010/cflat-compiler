# Copying one `unique` field into another double-frees in a GENERIC, but is diagnosed in a plain struct

Filed 2026-07-31 as the deliberate residue of
`unique-field-borrowed-param-not-diagnosed-in-generic`. **Pre-existing** - verified by direct
A/B on the master binary at `8c29ca7` and on that issue's fix branch: **exit 134 on BOTH**, so
that fix neither closes it nor worsens it.

Severity: **silent abort (exit 134), no diagnostic at all** on the generic spelling. The
program prints its output, then aborts in libmalloc during teardown. The asymmetry with the
plain spelling is the point - exactly the same shape as the issue this came out of.

## Repro - the two halves, verified side by side on `8c29ca7`

Generic field - **UNDIAGNOSED**, exit 134:

```cflat
struct Item { int v = default; };
struct Box<T> { T t = default; };
extern int main()
{
    Box<unique Item*> a = default;
    a.t = new Item();
    a.t->v = 4;
    Box<unique Item*> c = default;
    c.t = a.t;
    printf("f2f %d\n", c.t->v);
    return 0;
}
```
Prints `f2f 4`, then exit 134. Both `a` and `c` own the same allocation; both synthesized
destructors free it.

Plain struct field - **CORRECTLY DIAGNOSED**, exit 1:

```cflat
struct Item { int v = default; };
struct Holder { unique Item* slot = nullptr; };
extern int main()
{
    Holder a = default; a.slot = new Item(); a.slot->v = 4;
    Holder c = default; c.slot = a.slot;
    printf("plain %d\n", c.slot->v);
    return 0;
}
```
```
cannot store unique field 'a.slot' into unique field 'Holder.slot' - the source field's
synthesized destructor already frees it, and two 'unique' fields cannot own one pointer.
Use 'move a.slot' to transfer ownership out of the source field (which nulls it).
```

## Root cause - the SAME flag split, one leg short

This is not a missing feature. **The check and the message both already exist and are correct**;
they simply are not reached for a generic-substituted field.

Generic substitution sets `IsUniqueTypeArg`, never `IsUnique` (the latter is reserved for the
written qualifier). The fix for `unique-field-borrowed-param-not-diagnosed-in-generic` added
`IsOwningUniquePointerField` (`cflat/MainListener.h` ~10050) and re-keyed the borrow legs onto
it, but **deliberately left the field-to-field leg keyed on `IsUnique` alone**
(`cflat/MainListener.h` ~15730, "lifted out"). So the generic case falls straight through.

## The leg is TWO sub-cases - only one of them is left here

This distinction was established by A/B and by the round-1 review; get it right before editing,
because the two halves need different work.

**Sub-case A - MIXED (plain `unique` source into a GENERIC unique destination). ALREADY FIXED**
in the follow-up round of the borrowed-param work: only the DESTINATION gate was failing, and
re-keying it onto `IsOwningUniquePointerField` closed it with no source-side change.

**Sub-case B - FULLY GENERIC (both sides `IsUniqueTypeArg`). THIS ISSUE.** The repro at the top
of this file. Re-keying the destination cannot close it: the SOURCE gate `IsUniqueFieldRead`
(`cflat/MainListener.h` ~10042) independently requires `IsUnique` on the source, so the leg
short-circuits on the source no matter what the destination says.

> A note on the record: an earlier revision of this file claimed the "source predicate needs
> widening" reason was simply WRONG, on the evidence that the plain spelling is diagnosed. That
> overcorrected. The original deferral was **correct but incomplete** - correct about sub-case
> B, which is what remains here, and incomplete only in treating the leg as indivisible when
> sub-case A was closable on its own.

## Fix direction

Widen the SOURCE predicate `IsUniqueFieldRead` so a read of a generic-substituted owning field
(`IsUniqueTypeArg`, same shape gate as `IsOwningUniquePointerField`) counts as an
ownership-transferring read, then reuse the existing message verbatim so the plain and generic
spellings read identically.

This is the part with real blast radius: every read of a `unique` field flows through that
predicate, including the many legitimate ones (borrowing to call a method, passing to a reader,
comparing, returning a borrow). `move` must stay excluded, and self-assign must stay excluded.

Confirm the suggested remedy works before shipping the message: `c.t = move a.t;` must compile
and transfer for two `Box<unique Item*>`. **Verified working already** on `8c29ca7` - it
compiles, transfers, prints correctly, exit 0 - so the message's advice is sound.

**Guard polarity is load-bearing:** reject ONLY what you can PROVE. Legitimate reads of a
`unique` field (borrowing to call a method, passing to a reader, comparing, returning a borrow)
must stay legal - the leg fires only on a store INTO another owning field, which is provable
on both ends. See [`internal/fix-issue-lessons.md`](../../fix-issue-lessons.md).

Note there is a separate, larger recorded direction for `=` over `T` in
`internal/plan/ownership-transparent-assignment.md`. This issue is the narrow
consistency fix; check that plan before choosing to make `=` transfer implicitly instead.

## Test coverage

None for the generic half. The plain half is covered. Wants an `expect_error` leg in the
existing `Test/errors/err_unique_borrow_into_field.cb`.

Related: [[unique-unfreeable-address-residue]], [[assignment-transparency-direction]],
[[interface-issue-queue]]
