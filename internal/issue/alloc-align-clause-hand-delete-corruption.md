# `alignas(0,N)` on a hand-managed pointer field: `delete` silently becomes `_aligned_free`

> RESOLVED 2026-07-17 - `alignas(0,N)` on a non-`unique`, non-array-view pointer field is now a
> compile error (`err_alignas_alloc_requires_unique.cb`), verified green (test.bat 46/0). The corrupting
> repro now errors at compile time instead of exit 127. Safe to delete on commit.

Found 2026-07-16. Pre-existing; **knowingly left open** when the `unique` variant of this bug was
fixed the same day. Confirmed: exit 127, IR-verified.

## Summary

A field declared `alignas(0, N)` makes the compiler route `delete field` to `__delete_aligned`
(`_aligned_free`), recovering the allocation alignment from the field's clause. If the block stored
into that field came from a plain `operator new`, `_aligned_free` reads a header that was never
written -> heap corruption.

The store site now REJECTS this for `unique` fields
(see the fixed `alloc-align-clause-indirect-store-unchecked`), but the check is gated on `IsUnique`,
so an unmarked hand-managed field still compiles clean and still corrupts.

## Repro (`scratch/rv_align_ctl.cb`)

```cflat
struct Res { i64 v = 0; };
struct H
{
    alignas(0, 64) Res* p = nullptr;      // NOT unique - hand-managed
    ~H() { if (p != nullptr) delete p; }  // the user's own delete
};

extern int main()
{
    H h = default;
    Res* t = new Res();    // plain operator new - not 64-aligned, no aligned header
    h.p = t;               // compiles clean
}                          // ~H() -> delete p -> _aligned_free on a plain-new block
```

Exit 127. IR: **1x `call void @___delete_aligned_void_U8Ptr_`, 0x plain `operator delete`**.

## Why the `IsUnique` gate was accepted anyway

Without the gate, the store-site check produced a FALSE POSITIVE on legal code: a plain borrow field
with a clause, which frees nothing, was rejected with no followable remedy. The store site cannot
distinguish "this field's `delete` will be routed to `__delete_aligned`" from "this field is never
freed" - only the delete site knows.

So the gate is the right call at the store site, and this issue is the acknowledged cost.

## Why the rationale for accepting it was WRONG

The reasoning recorded at the time was: *"it is pre-existing, the user wrote the delete themselves,
and the bar this feature set is that SYNTHESIZED code should never need auditing."*

That does not survive the evidence. The user wrote `delete p`. They did **not** write, cannot see, and
cannot audit the compiler's decision to lower it to `_aligned_free` rather than `operator delete` -
that choice comes from the field's `alignas` clause. By the "synthesized code should never need
auditing" bar, this case arguably still qualifies: the freeing instruction is synthesized.

Recorded because it was nearly closed on a bad argument.

## DECISION (2026-07-17): `alignas(0,N)` requires `unique`

Make `alignas(0, N)` on a non-`unique`, non-array-view pointer field an ERROR. The clause only takes
effect if you also hand-write a `delete`, which is a trap either way; requiring `unique` gives the
store-site check the ownership information it needs, and `unique` fields already handle this
correctly. This is a language decision, not a bug fix - it is loud, and reversible. Chosen over the
delete-site-provenance option (which needs real new machinery: alignment provenance surviving the
store, which it does not today).

## Fix direction

**The real fix belongs at the DELETE site, not the store site.** The store site cannot know whether a
free will happen; the delete site knows exactly which free routine it is about to emit and what the
field's clause claims. Options:

- At the delete site, when a field's clause selects `__delete_aligned`, verify the block's provenance
  - which requires alignment provenance to survive the store (it does not today; a plain-`new` local
  carries `AllocAlignment == 0`).
- Or make the store-site check fire for any field with a clause whose `delete` COULD be routed -
  which needs a way to distinguish "freed through this field" from "never freed", i.e. exactly the
  ownership information `unique` provides. That may simply mean: **`alignas(0,N)` on a
  non-`unique`, non-array-view pointer field is a claim nothing backs, and should require `unique`.**
  That would be a language decision, not a bug fix.

The second option is worth considering seriously - a clause that only takes effect if you also
hand-write a `delete` is a trap either way.

## Related

- `internal/plan/field-ownership-unique.md` - the `unique` variant, fixed.
- `internal/unified-alignas-slot-alloc.md` - the `alignas(slot, alloc)` design.
