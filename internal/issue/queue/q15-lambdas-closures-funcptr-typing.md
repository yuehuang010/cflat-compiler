# q15: Lambdas, closures, and function-pointer typing

5 items. The type of a callable value is propagated on one call path and not the others, and a
null callable is accepted where it will be invoked unconditionally.

## Shared root cause

Callable typing is threaded through per-call-site state (`lastLambdaType`, `lambdaExpectedType`)
rather than derived from context. Any path that was not taught to set or scope that state either
loses the type or inherits the enclosing one. Separately, null-safety guards check the outer
handle but not the callable field inside it.

## Members

- `p2/lambda-expected-type-leaks-into-nested-literal` - a nested inferred lambda literal reads the
  ENCLOSING `lambdaExpectedType` instead of its own context.
- `p3/iface-arg-lambda-fnptr-type-not-propagated` - the interface-call argument loop lacks the
  direct call's `lastLambdaType` propagation step.
- `p2/c-binder-misses-decorated-function-pointer-parameter` - the `"(*)"` substring match misses
  clang's `"(* const)"` spelling, so the parameter binds as `void*`.
- `p3/nullptr-into-thin-funcptr-value-calls-null` - `nullptr` into a value (non-pointer)
  `function<T>` parameter compiles clean and null-calls at invocation.
- `ui/ui-boxed-closure-unguarded-null` - box-invoke helpers guard the box pointer but not the
  closure field inside it.

## Fix direction

1. Scope `lambdaExpectedType` per literal (save/restore on entry to a nested literal) instead of
   letting it leak; that is `p2/lambda-expected-type-leaks-into-nested-literal` and it removes a
   whole class of future drift.
2. Extract the direct call's callable-type propagation into a helper and call it from the
   interface-call argument loop too.
3. Reject `nullptr` into a value-typed `function<T>` at the call site, and null-check the closure
   field in the box-invoke helpers - both are missing guards, not typing.
4. Replace the `"(*)"` substring test in the C binder with a parse of clang's declarator rather
   than a wider substring set; the current approach will keep missing spellings.

Small and self-contained apart from item 1, which sits in shared lambda code.
