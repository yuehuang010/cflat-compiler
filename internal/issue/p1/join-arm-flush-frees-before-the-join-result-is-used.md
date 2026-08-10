# A join arm's owned-temp flush frees BEFORE the join result is used

Filed 2026-08-10 by `fix/coalarm`. P1: a USE-AFTER-FREE that compiles and reads freed memory.
Pre-existing on `?:` (measured on `0535f48`); `??` joined it when `fix/coalarm` gave the
fallback arm the same per-arm flush.

## What

A `?:` arm and (since `fix/coalarm`) a `??` fallback arm run `FlushOwnedTempsSince` INSIDE the
arm block, because the arm does not dominate the resume block and the end-of-statement
`FlushOwnedTemps` would otherwise skip - and leak - everything registered there.

That flush is EARLIER than the end of the full expression. For the DIRECT spelling the temp
lives until the statement boundary, which is after the use; through a join arm it dies at the
end of the arm, which is BEFORE the join result is used at all. Every consumer of the joined
value that is not covered by the escape guards (`RejectOwningTempUniqueFieldEscape`,
`RejectOwningTempUniqueFieldIntoSinkParam`) therefore reads freed memory.

## Repro

`scratch/ca_orc_t.cb`, `ca_orc_f.cb`, `ca_orc_wrap.cb` (`?:`, broken on master already) and
`scratch/ca_p3_read.cb`, `ca_t5.cb` (`??`, broken from `fix/coalarm` on):

```cflat
int dt = 0;
class Res { int v = 0; ~Res() { dt = dt + 1; } };
struct Box<T> { T t = default; };
Box<unique Res*> makeBox() { Box<unique Res*> b = default; b.t = new Res(); b.t->v = 70; return b; }
int readV(Res* p) { return p->v; }
Res* wrap(Res* p) { return p; }

int v = readV(makeBox().t);                  // 70,      dtors=1  - correct (direct)
int v = readV(c > 0 ? makeBox().t : n);      // GARBAGE, dtors=1  - '?:', broken on master
int v = readV(n ?? makeBox().t);             // GARBAGE, dtors=1  - '??', broken since fix/coalarm
Res* b = c > 0 ? wrap(makeBox().t) : n;      // GARBAGE, dtors=1  - '?:', broken on master
Res* b = n ?? wrap(makeBox().t);             // GARBAGE, dtors=1  - '??', broken since fix/coalarm
```

## Proof from `--no-opt` IR (`scratch/ca_orc_t.ll`, master binary)

```llvm
ternary_true:
  %2 = call %Box__unique_Resptr @_makeBox_...()
  %3 = extractvalue %Box__unique_Resptr %2, 0
  store %Box__unique_Resptr %2, ptr %owntemp
  call void @Box__unique_Resptr.dtorfull(ptr %owntemp)   ; <-- freed here
  br label %ternary_resume
ternary_resume:
  %ternary = phi ptr [ %3, %ternary_true ], [ %4, %ternary_false ]
  %5 = call i32 @_readV_i32_ResPtr_(ptr %ternary)        ; <-- used here
```

## Why `fix/coalarm` did not fix it

Its two covered consumers - storing the join into a local, and passing it to an
ownership-taking parameter - are REJECTED, and that rejection is what the arm flush is paired
with. The uncovered consumers are a plain-`T*` parameter read and a value laundered through a
borrowing callee, both of which the escape guards accept by design (the direct spelling of each
is correct and is frozen as an accept leg in `Test/test_move.cb`). Closing the hole needs one of:

- a "reached through a JOIN arm" predicate (not merely "not directly ledgered" - a CAST is not a
  join and does not move the free earlier) feeding a new rejection at those consumers, with its
  own accept set; or
- keeping the arm's temp alive to the statement boundary: spill it to an entry-block alloca that
  dominates the resume block, zero it at entry, and let the end-of-statement flush run the
  destructor on a zeroed record when the arm was not taken. Only alloca-based temps
  (`pendingOwnedStructTemps`) can do this; string / closure / ptr temps hold SSA values that are
  not referenceable from the resume block at all, which is why the per-arm flush exists.

The second is the real fix and is a redesign, not a patch.
