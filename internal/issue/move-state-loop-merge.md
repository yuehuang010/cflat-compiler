# Move-state (use-after-move) is not statically checked across loop back-edges

Status: KNOWN GAP, deferred. Diagnosed while designing the lock hand-over-hand
typestate (internal/plan/lock-hand-over-hand.md, internal/plan/lock-checker.md).

## Summary

Flow-sensitive move-state (NamedVariable.IsMoved / MovedFields) has snapshot/merge
for if/else (SaveMovedState / RestoreMovedState / MergeMovedStates,
LLVMBackend.h:14795-14826; call site MainListener.h if/else path), but loops
(while/for/do/for..in) and switch do NO snapshot/merge - ParseControlledBody
(MainListener.h:4664-5150) is a single linear walk. Static move-safety is therefore
NOT enforced across a loop back-edge. It appears to "work" only because owning storage
is physically nulled at runtime (a runtime crutch), so a use-after-move that the static
checker misses inside a loop still reads null rather than a dangling owner - masking the
missing static diagnostic rather than proving it sound.

## Repro direction (not yet minimized)

A `unique`/move value moved inside a loop body and then used on a later iteration (via
the back-edge) is not flagged statically. Construct: move out of a variable in the body,
use it at the top of the next iteration. Expected: "use of moved variable" error;
actual: no static error (runtime null masks it). TODO: minimize into a `Test/errors/`
case when this is fixed.

## Fix direction

DECIDED (maintainer): full-function MaybeInitialized dataflow fixpoint (Rust's model),
NOT a loop-scoped patch. Design and staged rollout in internal/plan/move-dataflow.md.
The merge is a MAY-analysis (moved on ANY incoming edge = union), the same solver the
lock held-state needs with the opposite meet (MUST/intersection) - build once, share.
Index/deref moves stay permissive (untracked). This is a behavioral TIGHTENING that will
surface currently-passing code, so it lands via the staged, verify-first rollout in that
plan, not as a drive-by.

## Related

- internal/plan/lock-hand-over-hand.md ("The one real gap" - the shared loop-edge mechanism)
- internal/plan/lock-checker.md (Tool C, open questions)
- internal/plan/ownership-sanitizer.md (M0 depends on this fix; move-state = MAY/union
  vs held-locks = MUST/intersection - same plumbing, opposite meet - build once, share)
