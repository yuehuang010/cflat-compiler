# q15: Lambdas, closures, and function-pointer typing

2 active items remain. The type of a callable value is propagated on one call path and not the others, and a
null callable is accepted where it will be invoked unconditionally.

## Shared root cause

Callable typing is threaded through per-call-site state (`lastLambdaType`, `lambdaExpectedType`)
rather than derived from context. Any path that was not taught to set or scope that state either
loses the type or inherits the enclosing one. Separately, null-safety guards check the outer
handle but not the callable field inside it.

## Members

- `p2/lambda-expected-type-leaks-into-nested-literal` - a nested inferred lambda literal reads the
  ENCLOSING `lambdaExpectedType` instead of its own context.
- Reviewed and closed as no-repro: interface-call lambda propagation has no demonstrated wrong
  overload or crash, and the existing callable suite remains green.
- `p3/nullptr-into-thin-funcptr-value-calls-null` - `nullptr` into a value (non-pointer)
  `function<T>` parameter compiles clean and null-calls at invocation.

## Fix direction

1. Scope `lambdaExpectedType` per literal (save/restore on entry to a nested literal) instead of
   letting it leak; that is `p2/lambda-expected-type-leaks-into-nested-literal` and it removes a
   whole class of future drift.
2. Extract the direct call's callable-type propagation into a helper and call it from the
   interface-call argument loop too.
3. Reject `nullptr` into a value-typed `function<T>` at the call site, and continue investigating
   the interface/lambda propagation shape only when a concrete wrong overload or crash is shown.

The C binder now parses the declarator rather than requiring the `"(*)"` substring, and boxed
closure helpers guard both the box and its closure field. Their issue files are deleted when fixed.

Small and self-contained apart from item 1, which sits in shared lambda code.
