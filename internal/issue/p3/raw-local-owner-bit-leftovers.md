# Raw-local owner-bit leftovers (T207 review round 1, note 3)

Bucket: p3. Found by the Opus review of T207 (scratch/briefs/t207_review1.md, branch
fix/t207-rawlaunder). All shapes behave the same on master and after T207; none is a regression.

- `raw = c ? p : nullptr; owner = raw;`, `raw = h.p` (h a local holding &s), and view->raw->owner
  are accepted on both; runtime-safe (no abort, dtor counts right) but not refused at compile time.
- (T207 r2 review) a local that starts owned, is borrowed, re-owned by `new` inside an inner block and consumed in that
  same block (`return raw;` / `owner = raw;` in the block) is refused (master compiled it, count 2). Matches the existing
  refusal for the same shape starting from nullptr; to accept, let the cross-block branch near
  MainListener_Expressions.cpp ~7524 treat the binding as owned for the rest of the current block.
## Left after T474 (conservative refusals; master aborted both at run time, 134)
- `for (..) { if (i == 0) owner = raw; raw = p; }` - the sink runs only on the first iteration; refused at the sink because
  the back-edge state (raw borrowed) reaches it on paper. Needs path sensitivity on the loop counter; accept only if cheap.
  The move form `if (i == 0) owner = move raw; raw = p;` (a17_cond) stays refused by the same rule.
