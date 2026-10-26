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

## T472 PARKED (2026-10-24, 3 Luna rounds; branch fix/t472-viewleak, r3 52c8f3161, NOT landed)
r1 added a run-time own bit on local aggregate array-view fields; r2/r3 disarm it on escape. r3 (load-based disarm, field
address recomputed at drop) fixes every r1/r2 review repro, but Opus review 3 (worktree scratch/briefs/t472_review3.md) blocks:
- REGRESSION: MainListener.h ~5801 read-only-method predicate rewritten (`!target.Borrow && (!target.Ok || !target.WholeLocal)`);
  `int peek() { auto c = a; c.p = b.q; return 1; }` on a const object is now refused (master + clang accept). Restore master's
  predicate; keep any "method hands out a view of its object" fact separate, read only by the disarm (Overloads ~8464).
- Getters returning a field view / element pointer (`S[] get() { return f; }`, `&f[0]`) on an aggregate whose field is tracked
  now dangle (UAF; master leaked): the call site keeps the release because the method is read-only.
  NEEDS RULING: (a) disarm when a method can return a view/pointer into its object (preferred: getters are common), or
  (b) accept like e12 (callee retains a borrowed view) and list it in borrowed-view-fields-passed-through-retaining-calls.md.
Repros scratch/repro_keep/box1022/t472/ (k2_const_alias_copy.cb, g06/g17/g18/g19).
Restart from the branch after the ruling: fix the two items above, re-run all rev_t472_* repro sets.
