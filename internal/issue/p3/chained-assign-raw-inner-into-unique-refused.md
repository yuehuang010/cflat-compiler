Bucket: p3 (usable-surface gap, conservative refusal; no wrong code)

# `owner = raw = new PS()` is refused while `raw = new PS(); owner = raw;` transfers ownership

Found 2026-09-24 by the review of fix/chained-pointer-assign (the fix that made chained assignment
produce the stored value instead of an invalid bitcast). With `unique PS* owner` and a raw
`PS* raw`, the chain is refused:

    cannot reset unique<PS> 'owner' from borrowed value 'raw'

while the two-statement equivalent compiles and consumes `raw` (a later read reports
`use of moved variable 'raw'`). Every other chained shape (local/global/field lhs, unique/unique,
raw lhs with unique inner, depth 3, scalars, `f(p = new PS())`, `if ((p = q) != nullptr)`)
matches its two-statement form. Probes: scratch/cpr/unique_lhs_raw_inner.* and
unique_lhs_raw_two.* in that branch's worktree (gone after merge; the two lines above repro it).

## Root cause

`finishStore` (cflat/MainListener_Expressions.cpp) reports a raw-pointer assignment result as a
borrow of the LHS, so the outer unique reset sees a borrowed value. The two-statement form reads
the NamedVariable `raw` itself, whose owning provenance lets the reset consume it.

## Fix direction

Make the inner assignment's result carry the LHS variable's identity (as if the outer RHS were the
bare identifier), so consume/move/borrow decisions and diagnostics are identical to `b = e; a = b;`.
A Codex round tried five variants (raw-only propagation, destination ownership refresh, stored-value
ownership evidence) without restoring move parity; one crashed test_move (139). Needs lldb.

## Acceptance

The chain compiles, runs one destructor, and a later read of `raw` reports use-after-move, exactly
as the two-statement form does.
