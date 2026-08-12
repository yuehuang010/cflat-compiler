# q10: `move` sinks and move spelling

BUCKET CLOSED 2026-08-12. Every actionable item landed; the 2 remaining members are RATIFIED
DEFERRALS with recorded rulings, not open work. This file stays only to carry those two forward.

Q10 fixed the indirect POD move false rejection, forward/local alias sink inference,
function-pointer allocation-alignment propagation, the lambda fresh-allocation diagnostic, and
closure return types accepting `move`/`alias` with ownership carried through synthesized invokers.

## Shared root cause (of what was fixed)

The affected paths had no single source of truth for ownership facts. The indirect call repeated
move-source bookkeeping instead of using the direct-call transfer routine, wrapper sink inference
compared raw spellings before all aliases were available, and a function-pointer signature dropped
allocation alignment while binding a named function. Separately, the lambda fresh-allocation
diagnostic assumed the named-function return syntax was available to a closure.

## Deferred members - do NOT re-open without a fresh ruling

- `p1/move-of-borrow-into-move-sink-parameter` - **RULED DEFER 2026-08-10 (maintainer).**
  `cflat/core/hpc/btree.cb` stays untouched and unknown-accepts holds. Deciding this cell now would
  either freeze a `move` contract around a btree already queued for rewrite, or force that rewrite
  ahead of the `list`/`dictionary` containers it is meant to imitate. Revisit AFTER those settle.
  The file also records two measured corrections to the original framing: `move` on a pointer
  parameter buys exactly ONE thing - permission to `delete` - and gutting a pointee's owning fields
  is already legal through a plain borrow parameter.
- `p2/deref-of-moved-pointer-guard-inside-callee` - the conditional-termination half only. The
  cross-block diagnostic in `cflat/MoveDataflow.h` is intraprocedural, so a guard that prevents the
  dereference by calling a conditionally-terminating callee is invisible to it. Filed by design;
  closing it means interprocedural termination analysis.

## Fixed in Q10

- `p3/indirect-call-marks-a-pod-move-argument-moved-but-the-direct-call-does-not`
- `p2/forward-or-local-alias-in-cast-defeats-owning-sink-inference`
- `p3/funcptr-type-cannot-record-a-move-param-alloc-alignment`
- Closure return types accept `move`/`alias` qualifiers.

## Standing constraint

Explicit `move x` nulls the source and leaves it READABLE AS NULL, by design. Do not "fix" any of
these by importing Rust move semantics - that has been tried and breaks the suite.
