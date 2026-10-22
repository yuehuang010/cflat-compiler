# An owned view overwritten in place, or left in a local aggregate's field, leaks

Pre-existing on master; found by the T387 Sol review round 3 (2026-10-21). HeapAudit counts identical on master and T387.

## Repros (scratch/repro_keep/box1021/t387_rev3/)

1. `leak_overwrite.cb`: `for (...) { S[] v = new S[1]; if (c) v = new S[2]; ...; sink(move v); }` - at c = 1 the first `new S[1]` is
   never freed when `v` is re-assigned (HeapAudit leaks 2 over 2 iterations; 0 at c = 0).
2. `leak_shadow_outer.cb`: `HA a; a.hs[0].f = argc > 1 ? new S[1] : w;` never sunk - at argc > 1 the owned arm is not freed when `a`
   leaves scope (HeapAudit leaks 1).

## Notes

T387's accept legs checkT387LoopInner / checkT387ScopeShadowKey in Test/test_move.cb omit leak assertions because of these leaks; add
them when this is fixed. Check first whether `=` over an owning `S[]` local is meant to release the old block (assignment transparency:
`=` is total over T) - case 1 suggests the release is skipped when the slot's ownership is decided at run time.
