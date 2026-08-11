# q05: Unique/owning assignment arm

6 items. `=` over an owning or unique-field-bearing value takes an arm that bit-copies, or that
drops the old value, without consulting the provenance of what it is copying.

## Shared root cause

Assignment is a set of special-cased arms rather than one operation total over `T`. Each arm makes
its own decision about copy vs move vs drop-old, and several never look at per-field unique
provenance at all. This is the exact problem `internal/plan/ownership-transparent-assignment.md`
(Option 5: `=` total over `T`) exists to fix, and four of the six were filed against that plan.

## Members

- `p1/unique-assign-syntactic-owned-rhs-leaks` - `llvm.mem*` destination-side store of a
  caller-owned pointer is never checked, only the source side.
- `p2/owning-local-assign-arm-accepts-a-mismatched-source-type` - reassignment arm destructs using
  one type name but never compares it to the source's type name.
- `p2/direct-unique-field-read-into-a-unique-local` - the unique-local door only rejects when
  `IsBorrowed` is set; a direct field read never sets it.
- `p2/ternary-join-unique-field-store` - the join strips `IsUniqueFieldRead`, so the store sees an
  ordinary pointer and emits no diagnostic.
- `p2/whole-struct-copy-aliases-a-unique-field` - whole-struct `=` bit-copies the aggregate
  including a unique field; the per-field move/drop-old paths are never consulted.
- `p3/self-assign-through-a-pointer-to-the-destination-drops-the-value` - the identity guard
  compares the loaded value, not the destination alloca, for deref sources.

## Fix direction

Do AFTER q01 (the arms read the owned-temp ledgers, and presence currently fails unsafe).

Follow the transparent-assignment plan: one entry point that, for any `T`, walks fields and picks
copy / move / drop-old per field from provenance, with the self-assign identity check done on the
DESTINATION ALLOCA up front. The whole-struct case then stops being a separate arm.

`p2/ternary-join-unique-field-store` needs q02's classifier to keep `IsUniqueFieldRead` alive
across the join; sequence it after q02.

## Adjacent

q06 (the same provenance facts, lost at a different hop), q02.
