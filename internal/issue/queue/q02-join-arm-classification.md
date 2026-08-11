# q02: Join-arm classification (is/as, `??`, ternary, interface boxing)

5 items. Every site that asks "what is the static class/type of this value?" re-derives the answer
from the IR shape it happens to be handed, and each site recognizes a different subset of shapes.

## Shared root cause

There is no single "given a value produced by a join, what did the source spell?" helper. Instead:

- the is/as cast classifier only recognizes a `PHINode` as a join, so `??` (which lowers through a
  slot plus a `LoadInst`) is not seen as one;
- interface-boxing class resolution reads the declared type off a `LoadInst`, so a direct call
  result (not a load) resolves to empty;
- ternary-arm resolution builds positional arrays and cannot fold a nested join arm at all;
- box registration dedupes on `FatValue` alone, losing the source binding.

## Members

- `p2/as-is-does-not-recognize-nullcoalesce-join`
- `p2/move-interface-return-of-nullcoalesce-join-not-owned`
- `p2/nested-join-arm-unresolved-in-is-as-and-mixed-ternary`
- `p2/join-arm-from-call-result-not-boxed-into-interface`
- `p3/interface-boxing-keyed-on-source-binding`

## Fix direction

Introduce one `ResolveJoinArmClass(Value*)` (or extend the existing arm resolver) that handles
PHI, slot+load, direct call result, and recursively a nested join, and returns both the class name
and the source binding. Then convert the four sites to call it. Key box registration on
`(FatValue, DataPointer, Source)`.

Verify with a matrix test in an existing `Test/test_interface*.cb` covering: `??` join, ternary
join, nested ternary, direct-call arm, each fed into `is`, `as`, an interface parameter, and a
`move` return.

## Adjacent

q05 (`p2/ternary-join-unique-field-store` - join strips field provenance), q07
(`p3/boxed-join-proof-never-retires-a-rebound-arm`). Both get easier once the classifier exists,
but they are ownership-fact problems, not classification problems, so they stay in their buckets.
