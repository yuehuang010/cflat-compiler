# Consuming a void closure call's result reads an uninitialised register

Filed 2026-08-09 during the verification round of the void expression-body lambda fix. This is
**pre-existing on master** - measured identical on the merge base `4565f1e` and on the branch for
every spelling that already compiled - but the fix makes one more spelling reach it, so it is
recorded here rather than left undiscovered.

Severity: **P1, SILENT WRONG VALUE.** No diagnostic, no verifier failure, exit 0.

## Repro

```cflat
void bump() { }
extern int main()
{
    Lambda<void()> g = () => { bump(); };
    int r = g();                 // prints r=-16
    printf("r=%d\n", r);
    return 0;
}
```

Measured PRE and POST, byte-identical:

| spelling | PRE | POST |
|---|---|---|
| `Lambda<void()> g = () => { bump(); }; int r = g();` | `r=-16` | `r=-16` |
| `function<void()> g = () => { bump(); }; int r = g();` | `r=-77135616` | `r=-77135616` |
| `Lambda<void()> g = bump; int r = g();` | `r=-16` | `r=-16` |
| `Lambda<void()> g = () => { x = 5; }; int r = g();` | `r=-16 x=5` | `r=-16 x=5` |

The value is an uninitialised register, not a defined one - the thin and fat spellings read
different garbage.

## The direct (non-closure) spelling is LOUD, which is the whole asymmetry

```cflat
void bump() { }
int r = bump();      // module verification failed, rc 1, no binary - BOTH binaries
```

So the language already refuses to bind a void call's result through the direct path, and only
the CLOSURE call path lowers it into garbage. `CreateIndirectCall` / the closure invoke hands back
a value for a void-returning invoker instead of a void, so the declarator's store never trips the
type check that catches the direct spelling.

## What the void expression-body fix changed here

Nothing about the defect; only its reachability. Before the fix `Lambda<void()> g = () => bump();`
failed module verification, so its `int r = g();` could not be reached at all. After the fix that
spelling compiles like its block-body twin and reaches the same pre-existing hole, with the same
value. The convergence is the point of the fix; the hole underneath it is this issue.

Two immediately-invoked spellings land here too, through the inferred-return path:

```cflat
int r = (() => bump())();      // r=-16   (void-yielding body, genuine discard, result consumed)
int r = (() => (x = 5))();     // r=-16 x=5
```

Their value-yielding siblings (`((int x) => x * 2)(4)`) are diagnosed by
`cannot infer the return type of lambda ...`; these two are not, because their body really is
void-yielding and the discard is correct - the defect is at the consumption, not the body.

## Fix direction

Make a void-returning closure invocation yield a void value, so every consumer fails exactly where
the direct spelling already fails; then convert that shared failure into a located diagnostic as
part of [[return-value-void-mismatch-fails-module-verification]] Family B, which is the same
question in return position. Do NOT special-case the declarator - the accept set is every
consumption position (declarator init, assignment, argument, return, field store, join arm), and a
per-site rule would be the site-enumeration failure mode this repo has paid for repeatedly.
