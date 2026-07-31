# `Box<function<int(int)>>` - a generic wrapper over a function type - raises an LLVM fatal

Filed 2026-07-31 by the review of `function-array-body-silently-truncated`. **Pre-existing**,
verified on the master binary at `64b6118`, and provably untouched by that fix (both of its
changes are gated on an array suffix).

Severity: hard failure with **no usable source diagnostic**. The only output is an LLVM
fatal plus the `CompilerManager` state dump, which names no source location. Nothing
miscompiles, so this is a feature gap plus a diagnostic failure rather than a wrong program -
hence P2 rather than P1, though it is close to the line.

## Repro

```cflat
import "function.cb";
struct Box<T> { T value = default; };
int triple(int x) { return x * 3; }
extern int main()
{
    Box<function<int(int)>> b = default;
    b.value = triple;
    printf("%d\n", b.value(2));
    return 0;
}
```

Observed on master:

```
LLVM ERROR: Cannot select: AArch64ISD::CALL
... CompilerManager state dump: Function: main / Scope depth: 0 / Structs registered: 40 ...
```
exit **134**.

Note the substitution and the store are fine; it is the INVOKE through the generic-substituted
field (`b.value(2)`) that fails instruction selection. A `Box<function<...>>` that is only
stored and never invoked should be checked - if that compiles, the gap is specifically the
call lowering.

## Why it must not stay an LLVM fatal

Per CLAUDE.md's debugging convention, an LLVM assert or fatal reachable from plain source must
become a proper compile-time error once diagnosed. Whatever the outcome on supporting the
feature, the fatal has to be replaced by a `LogError` naming the source construct.

## Fix direction

Not investigated. Two acceptable outcomes:

1. **Support it** - make the generic-substituted function-typed field lower to a real
   thin/fat closure call, the way a non-generic `function<T>` field already does.
2. **Reject it** - `LogError` at the declaration or the call, naming the construct, if
   supporting generic wrappers over function types is a real feature project.

Start by comparing the substituted field's `TypeAndValue` against the non-generic
`struct S { function<int(int)> f = default; }` case, which works - the thin/fat closure
distinction is the most likely thing lost in substitution.

## Test coverage

None. Extend `Test/test_function_ptr.cb` if supported, or add a `Test/errors/err_*.cb` leg if
rejected.

Related: [[interface-issue-queue]]
