# An owning temp in a `??` FALLBACK arm is never destructed

Filed 2026-08-05 by `fix/tempuniq`, whose join walk had to EXCLUDE this arm to avoid a false
rejection. P2: a LEAK, not a wrong value - but it is load-bearing for a memory-safety guard, so
fixing it has a second half (see "Coupled to" below).

## What

The `??` lowering (`ParseConditionalExpression`, `cflat/MainListener.h`) evaluates its right
operand inside a `nullcoal_null` block:

```
CreateConditionJump(lhs, notNullBlock, nullBlock)
  notNullBlock:  store lhs   -> resultAlloca
  nullBlock:     <RHS evaluated here>  store rhs -> resultAlloca
  resumeBlock:   joined = load resultAlloca
```

`nullcoal_null` does not dominate `resumeBlock`, so the end-of-statement `FlushOwnedTemps` skips
every owned temp registered there (`OwnedTempDominatesHere`, `cflat/LLVMBackend.h`). Unlike the
`?:` path - which calls `FlushOwnedTempsSince` INSIDE each arm block for exactly this reason, and
says so in that function's own comment - the `??` path makes no such call. The temp is therefore
never destructed at all.

## Repro

Identical on `14097e1` and on the merged `fix/tempuniq` (`scratch/tu/arm.cb`):

```cflat
int dt = 0;
class Res { int v = 0; ~Res() { dt = dt + 1; } };
struct Box<T> { T t = default; };
Box<unique Res*> makeBox() { Box<unique Res*> b = default; b.t = new Res(); b.t->v = 70; return b; }

Res* n = nullptr;
dt = 0; Res* a = makeBox().t ?? nullptr;   // LHS arm: v=garbage, dtors=1  (freed - correct)
dt = 0; Res* b = n ?? makeBox().t;         // RHS arm: v=70,      dtors=0  (never freed - LEAK)
```

The same measurement on `?:` gives `dtors=1` for BOTH the true arm and a taken false arm, which is
what proves the defect is specific to `??`'s fallback arm and not to joins generally.

## Coupled to the escape guard - both halves must land together

`fix/tempuniq` closed "a temp's `unique` field escaping through a join" by walking the join arms.
The RHS arm had to be excluded from that walk (`JoinCarriesOwningTempUniqueField`,
`cflat/LLVMBackend.h`): with the temp never destructed there is no dangle, so rejecting it would
refuse a program master compiles and runs correctly - the polarity rule this repo has paid for
repeatedly.

**Fixing this leak turns that shape into a genuine use-after-free.** So the fix is two halves:
call `FlushOwnedTempsSince` for the `??` fallback arm the way the `?:` path already does, AND
delete the arm-0-only restriction in `JoinCarriesOwningTempUniqueField` in the same change. The
comment at that function points back here.

## Neighbours with the same root

Not the same issue, but the same "an owning temp in a block that never gets flushed is never
destructed" shape, so a fix here should be probed against both:

- `lambda-body-owning-temp-never-destructed` (fixed and deleted by `fix/lamtemp`, 2026-08-09) -
  an EXPRESSION-BODY lambda emitted its `ret` directly and so ran neither the return path's
  owned-temp flush nor its escape gates; a block-body lambda always did. Fixed by routing
  `=> expr` through the shared `MainListener::EmitReturnExpression`.
- `coalesce-assign-skips-store-bookkeeping` (fixed and deleted by `fix/coalesce-tail`) -
  `raw ??= makeBox().t` measured `dtors=0` for the same reason plus its own skipped store tail;
  that spelling is a hard error now, so this `??` fallback-arm leak is the surviving half.

## Round-1 review addendum (2026-08-05)

`sink(p ?? makeBox().t)` with a `unique` sink parameter is ACCEPTED and measures `dtors=1` on
both binaries - the callee's scope-exit free is currently the only free, so it happens to run
correctly. When the leak fix lands and the fallback arm's temp starts being destructed, that
shape becomes a DOUBLE free, not just a UAF. Probe it in the same change that deletes the
`Arms[0]`-only exclusion; the `..._fallback_arm_not_freed` accept leg (pinning `dtorCount == 0`)
is the tripwire that forces this file to be revisited.

Related: [[interface-issue-queue]]
