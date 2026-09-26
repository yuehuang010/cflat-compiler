# `?:` arm holding `&(new T)->f ?? x` frees the temporary before the call

Bucket: p2 (use-after-free in the callee). Pre-existing: same early free before and after
b3a6e009, which fixed the leak for arms whose `new` is the whole arm or the `??` RHS.

## Repro

`borrow(c > 0 ? &(new T(21))->v ?? nullptr : &(new T(22))->v);` - `borrow` sees the destructor
already ran. Probe: scratch/argleak_review/conditional_new_review.cb case 9.

## Root cause (suspected)

The inner `??` flushes its LHS temp at its own join; only the RHS goes through the conditional
slot (HoistOwnedPtrTempsForAddress, includeBareNew path). The enclosing `?:` arm then never sees it.

## Fix direction

Hoist the `??` LHS temp into a conditional slot too when inCallArgument_, so the enclosing
ternary join carries it to the after-call cleanup.
