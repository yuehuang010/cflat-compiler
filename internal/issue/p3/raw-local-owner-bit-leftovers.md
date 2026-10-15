# Raw-local owner-bit leftovers (T207 review round 1, note 3)

Bucket: p3. Found by the Opus review of T207 (scratch/briefs/t207_review1.md, branch
fix/t207-rawlaunder). All shapes behave the same on master and after T207; none is a regression.

- `raw = new R(); for (2) { owner = raw; raw = p; }` aborts 134 (single pass, back edge after use).
- `R[] v = arr; raw = v; delete raw;` aborts 134 (delete through a view-derived raw).
- `raw = c ? p : nullptr; owner = raw;`, `raw = h.p` (h a local holding &s), and view->raw->owner
  are accepted on both; runtime-safe (no abort, dtor counts right) but not refused at compile time.
- `move R* m` reassigned by `m = new R();` does not release the moved-in object. Codified by the
  raw-count legs in Test/test_core.cb (runRawCountMoveParamReassign*): the expected counts exclude
  the old array. Needs a maintainer ruling before any change; the T207 borrow path releases its own.
- (T207 r2 review) a local that starts owned, is borrowed, re-owned by `new` inside an inner block and consumed in that
  same block (`return raw;` / `owner = raw;` in the block) is refused (master compiled it, count 2). Matches the existing
  refusal for the same shape starting from nullptr; to accept, let the cross-block branch near
  MainListener_Expressions.cpp ~7524 treat the binding as owned for the rest of the current block.
