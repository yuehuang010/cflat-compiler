# `sizeof(T[N])` is parsed as a cast and rejected with a message about casts

Filed 2026-07-31, found while verifying the multi-dimensional array work in
`function-array-body-silently-truncated`. **Pre-existing**, verified on the master binary at
`64b6118`. Not specific to function types or to multiple dimensions.

Severity: a legal-looking spelling is unavailable, and the diagnostic blames a construct the
user did not write. No wrong values - `sizeof` on a VARIABLE works correctly, so there is a
working spelling.

## Repro

```cflat
extern int main() { printf("%d\n", (int)sizeof(int[2][3])); return 0; }
```
```
sizeof_type.cb(1,47): a sized array '(T[N])' is not a valid cast target; use '(T[])' for the
noalias array-view
```

One dimension fails identically, so this is not about multi-dim:

```cflat
extern int main() { printf("%d\n", (int)sizeof(int[3])); return 0; }
```
same message.

The workaround is `sizeof(someVariable)`, which reports correct sizes (verified: a
`function<int(int)>[2][3]` variable reports 48, `int[2][3]` reports 24).

## Why the message is the worst part

The user wrote `sizeof`, not a cast. The parser reaches the cast rule because `(` `type` `)`
is tried first, so the error text describes a construct that is not in the source. Someone
hitting this has no path from the message to the workaround.

## Fix direction

Not investigated. Two parts, and the second is worth doing even if the first is deferred:

1. Let `sizeof` take a sized-array TYPE operand, resolving the `(T[N])` ambiguity in favour of
   `sizeof` when the token before `(` is `sizeof`.
2. **At minimum**, when the cast-target rejection fires inside a `sizeof` operand, say so -
   name `sizeof` and point at `sizeof(<variable>)`. A misleading message is worse than a
   missing feature.

## Test coverage

None. Wants a `Test/errors/err_*.cb` leg if it stays rejected, or a value assertion in an
existing test if `sizeof(T[N])` becomes legal.

Related: [[interface-issue-queue]]
