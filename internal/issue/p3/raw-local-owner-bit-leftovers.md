# Raw-local owner-bit leftovers (T207 review round 1, note 3)

Bucket: p3. Found by the Opus review of T207 (scratch/briefs/t207_review1.md, branch
fix/t207-rawlaunder). All shapes behave the same on master and after T207; none is a regression.

- `raw = c ? p : nullptr; owner = raw;`, `raw = h.p` (h a local holding &s), and view->raw->owner
  are accepted on both; runtime-safe (no abort, dtor counts right) but not refused at compile time.
- `move R* m` reassigned by `m = new R();` does not release the moved-in object. Codified by the
  raw-count legs in Test/test_core.cb (runRawCountMoveParamReassign*): the expected counts exclude
  the old array. Needs a maintainer ruling before any change; the T207 borrow path releases its own.
- (T207 r2 review) a local that starts owned, is borrowed, re-owned by `new` inside an inner block and consumed in that
  same block (`return raw;` / `owner = raw;` in the block) is refused (master compiled it, count 2). Matches the existing
  refusal for the same shape starting from nullptr; to accept, let the cross-block branch near
  MainListener_Expressions.cpp ~7524 treat the binding as owned for the rest of the current block.
- (T474 review round 1 finding 4) After `if (condition) { raw = p; } raw = new R();`, the borrowed state can remain
  sticky after the fresh owning assignment, so a later `delete raw` is refused as an alias of `p`. Repro:
  `if_borrow_then_new` in `scratch/rev_t474_opus_probe.sh`. Fix direction: retire the borrow provenance only when the
  resolved assignment source proves a fresh owner, including after the branch join; cover the `break` arm separately.
- (T474 review round 1 move-sink follow-up) The repeated-loop `owner = move raw; raw = p;` repro still compiles and
  ASan aborts when the second iteration moves the borrowed stack pointer. The assignment path's move result is detached
  (`Storage == nullptr`); its original slot is carried separately in `MoveSourceStorage`, so the loop sink recorder misses
  the resolved local. Fix direction: carry that storage identity through the unique/move sink recording path and retain
  the back-edge refusal when the later sink can read a borrowed value. Repro: `scratch/rev_t474_opus_move_assign.cb`.

## Left after T474 (conservative refusals; master aborted both at run time, 134)
- `for (..) { if (i == 0) owner = raw; raw = p; }` - the sink runs only on the first iteration; refused at the sink because
  the back-edge state (raw borrowed) reaches it on paper. Needs path sensitivity on the loop counter; accept only if cheap.
- `for (..) { owner = raw; if (i == 0) { raw = p; break; } raw = new R(); }` - the borrowed state leaves through `break`
  and never reaches the back edge, yet the back-edge check sees it: break-path state must not merge into the back-edge
  state. A real false refusal; fix in the loop frame join (LLVMBackend_OwnershipTemps.cpp loop entry/exit snapshots).

## Ruled 2026-10-25: `move R* m` parameter reassigned by `m = new R();`
Maintainer ruling (with the R9 hold lift): the assignment RELEASES the moved-in object (destructor + free, honouring the
hidden raw-array count) before storing the new value - assignment transparency, same as any owning slot. The raw-count legs
that pin the leak today are updated with the fix. Buildable now.
