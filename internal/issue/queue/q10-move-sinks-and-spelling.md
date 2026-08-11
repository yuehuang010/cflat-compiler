# q10: `move` sinks and move spelling

6 items. What counts as an owning sink, and what a `move` parameter accepts, is decided
inconsistently between the direct-call path, the indirect-call path, and the forward-ref scanner -
and some legal intents cannot be spelled at all.

## Shared root cause

Two halves:

- **Inconsistent sink inference.** The direct call, the indirect call, and `ForwardRefScanner`
  each compute "is this an owning sink" from different inputs (canonical name intersection, an
  owning-type gate, a text match). Any input the other side cannot see - a local `using` alias, a
  callee-internal guard, a POD type - produces a disagreement.
- **Unspellable types.** The grammar cannot express `move T*` / `alias T*` as a lambda return
  type, and the funcptr type has no field for a move parameter's alloc alignment, so a diagnostic
  suggests a remedy that does not compile.

## Members

- `p1/move-of-borrow-into-move-sink-parameter` - a callee's `move` parameter accepts a moved
  borrow and frees caller-owned memory.
- `p3/indirect-call-marks-a-pod-move-argument-moved-but-the-direct-call-does-not` - indirect path
  lacks the direct path's owning-type gate.
- `p2/deref-of-moved-pointer-guard-inside-callee` - intraprocedural analysis cannot see a
  conditionally-terminating guard inside the callee body.
- `p2/forward-or-local-alias-in-cast-defeats-owning-sink-inference` - `ForwardRefScanner` does not
  see a forward or local `using` alias, so canonical-name intersection misses.
- `p2/lambda-return-type-cannot-be-spelled-move-or-alias`
- `p3/funcptr-type-cannot-record-a-move-param-alloc-alignment`

## Fix direction

1. Extract ONE `IsOwningSink(param)` used by the direct call, the indirect call, and the scanner.
   The POD gate and the alias resolution then apply everywhere by construction. Note the
   both-passes rule: any change to `ParseDeclarationSpecifiers` must land in BOTH copies in
   `MainListener.h`.
2. `p1/move-of-borrow-into-move-sink-parameter` is the acceptance check on the callee side and can
   be fixed independently - do it first, it is the p1.
3. The two spelling gaps are grammar work (`CFlat.g4` plus a funcptr type field) and are
   independent of the inference work; they can go to a separate agent.

Note the design record: explicit `move x` nulls the source and leaves it readable as null BY
DESIGN. Do not "fix" any of these by importing Rust move semantics - that has been tried and
breaks the suite.
