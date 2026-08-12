# q10: `move` sinks and move spelling

2 active items remain. What counts as an owning sink, and what a `move` parameter accepts, is decided
inconsistently between the direct-call path, the indirect-call path, and the forward-ref scanner -
and some legal intents cannot be spelled at all. Q10 fixed the indirect POD move false rejection,
forward/local alias sink inference, function-pointer allocation-alignment propagation, and the
lambda fresh-allocation diagnostic. The remaining items are deferred behavior or a language-design
question.

## Shared root cause

The affected paths had no single source of truth for ownership facts. The indirect call repeated
move-source bookkeeping instead of using the direct-call transfer routine, wrapper sink inference
compared raw spellings before all aliases were available, and a function-pointer signature dropped
allocation alignment while binding a named function. Separately, the lambda fresh-allocation
diagnostic assumed the named-function return syntax was available to a closure.

## Members

- `p1/move-of-borrow-into-move-sink-parameter` - a callee's `move` parameter accepts a moved
  borrow and frees caller-owned memory.
- `p2/deref-of-moved-pointer-guard-inside-callee` - intraprocedural analysis cannot see a
  conditionally-terminating guard inside the callee body.
- Fixed: closure return types accept `move` and `alias` qualifiers and propagate ownership through
  synthesized invokers.
- `p1/move-of-borrow-into-move-sink-parameter` - a design/ownership rule intentionally remains
  deferred.

Fixed in Q10:

- `p3/indirect-call-marks-a-pod-move-argument-moved-but-the-direct-call-does-not`
- `p2/forward-or-local-alias-in-cast-defeats-owning-sink-inference`
- `p3/funcptr-type-cannot-record-a-move-param-alloc-alignment`

## Fix direction

1. The indirect-call path now delegates ownership transfer to the same move-param transfer used by
   direct calls, so POD arguments stay readable and owning arguments retain the established path.
2. Wrapper sink inference canonicalizes pure rename aliases and refreshes the emitted function
   symbol after the main body walk, which covers file-forward and function-local aliases without
   changing the type-changing-cast accept set.
3. Function-pointer parameter signatures carry allocation alignment through binding, agreement
   checks, and both cache serializers. Indirect calls therefore reject an over-aligned allocation
   when the named sink does not carry the matching clause.
4. The lambda diagnostic now gives a compilable named-function remedy. Closure return qualifiers
   are now accepted and carried through indirect calls.

`p1/move-of-borrow-into-move-sink-parameter` and the conditional-termination half of
`p2/deref-of-moved-pointer-guard-inside-callee` remain filed by design.

Note the design record: explicit `move x` nulls the source and leaves it readable as null BY
DESIGN. Do not "fix" any of these by importing Rust move semantics - that has been tried and
breaks the suite.
