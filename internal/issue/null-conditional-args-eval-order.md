# '?.' call arguments evaluate before the null-guard branch

Found 2026-07-26 during opus review of the whole-chain '?.' short-circuit work (commit
129cd2c and predecessors). Deferred - not fixed.

## Repro

```cflat
struct Node
{
    int v = default;
    int add(int a) { return v + a; }
};
int sideEffect(int x) { printf("sideEffect(%d) called\n", x); return x; }

extern int main()
{
    Node* z = nullptr;
    int r = z?.add(sideEffect(5));  // sideEffect(5) runs anyway; r is still 0
    printf("r=%d\n", r);
    return 0;
}
```

`sideEffect(5)` prints even though `z` is null; the final result `r` is still correctly 0.

## Root cause

Every null-conditional call site (interface method, struct method, extension function)
parses/evaluates its argument list via the shared call-argument-collection code BEFORE
`ncEnterGuard` (or, for the original single-link '?.' sites predating the chain fix, the
older per-link null check) ever branches on the receiver. The receiver test happens only
once dispatch begins, so a side-effecting argument expression always runs.

## Fix direction

Re-order argument evaluation to happen inside the guarded "access" block, after the null
branch - this touches the shared argument-collection path used by every call site
(guarded and unguarded), not just the '?.' arms, so it needs care to avoid perturbing
non-null-conditional calls. Alternatively, accept eagerly-evaluated '?.' arguments as a
documented part of the language's short-circuit contract (side-effect-free arguments
recommended in '?.' call position).
