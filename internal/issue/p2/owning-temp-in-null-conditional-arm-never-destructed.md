# An owning temp in a `?.` GUARDED chain is never destructed

Filed 2026-08-10 by `fix/coalarm`, which fixed the same root cause in the `??` fallback arm and
in the `&&` / `||` short-circuit blocks and measured this third site left over. P2: a LEAK, not
a wrong value.

## What

The whole-chain `?.` short-circuit (`MainListener_PostfixExpression.cpp`, "Whole-chain '?.'
short-circuit: links after the first '?.' run in a shared access block") evaluates the rest of
the chain - including its ARGUMENTS - in a guarded block that does not dominate the resume
block. `FlushOwnedTemps` at the block-item boundary therefore skips every owned temp registered
there (`OwnedTempDominatesHere`, `cflat/LLVMBackend.h`), exactly as the `??` fallback arm did.

## Repro (`scratch/ca_qdot3.cb`, `scratch/ca_qdot4.cb`)

```cflat
int dt = 0;
class Res { int v = 0; ~Res() { dt = dt + 1; } };
struct Box<T> { T t = default; };
Box<unique Res*> makeBox() { Box<unique Res*> b = default; b.t = new Res(); b.t->v = 70; return b; }
class H { int add(int a) { return a + 1; } };

H* h = new H();
dt = 0; int v = h?.add(makeBox().t->v);   // v=71, dtors=0  - LEAK
dt = 0; int v = h.add(makeBox().t->v);    // v=71, dtors=1  - correct
```

Identical on `0535f48` and on the merged `fix/coalarm` binary.

## Fix direction

The same one-line mirror the other three sites use: `MarkOwnedTemps()` before the guarded chain
is walked, `FlushOwnedTempsSince(mark, <yielded value>)` at the end of the guarded block, before
the branch to the resume block. The reason it was not done in `fix/coalarm` is surface, not
doubt: the guarded region is threaded through the 4000-line postfix walker across several
link kinds, so the mark/flush pair has to be placed once per exit edge rather than once per
lowering function, and each exit needs its own measurement.

## Tripwire

`Test/test_move.cb` pins the leak on purpose:
`null_conditional_arm_temp_not_freed` asserts `dtorCount == 0`. It MUST flip to `1` when this is
fixed; that flip is the leg doing its job, not a weakened assertion.
