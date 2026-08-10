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

## Widened by `fix/joinlife` (2026-08-10): a JOIN inside a `?.` arm now leaks too

`fix/joinlife` stopped `?:` / `??` arms from destructing an alloca-based owning struct temp
INSIDE the arm - it is zeroed at the branch and destructed at the statement boundary instead.
A join nested in a `?.` guarded arm therefore re-keys its temp to the join's own branch block,
which still sits inside the guarded region, so this leak now swallows it as well:

```cflat
Resource* live = new Resource();
int got = live?.readId(readResourceId(c > 0 ? makeMoveBox().t : nul));
// pre-joinlife: got = GARBAGE (freed in the arm, read in the resume), dtors = 1
// now:          got = 44 (correct),                                   dtors = 0  - LEAK
```

That is a use-after-free traded for a leak, the direction this repo takes deliberately, and it
disappears with this issue. It is worth exactly +1 leak / +16 bytes on `Test/test_move.cb` under
`leaks --atExit` (17/336 -> 18/352), all of it the one `null_conditional_join_arm` leg.

## Tripwire

`Test/test_move.cb` pins the leak on purpose:
`null_conditional_arm_temp_not_freed` and `null_conditional_join_arm_not_freed` both assert
`dtorCount == 0`. Both MUST flip to `1` when this is fixed; that flip is the legs doing their
job, not a weakened assertion.
