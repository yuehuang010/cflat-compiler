# `??=` returns before the plain-`=` store bookkeeping

P2, PRE-EXISTING (not introduced by the interface-boxing guard). Filed 2026-08-04 while fixing the
`??=` half of [[interface-boxing-keyed-on-source-binding]].

## What

`ParseAssignmentExpression`'s `??=` handler (`cflat/MainListener.h`, the
`if (operatorText == "??=")` branch) emits its own compare/branch/store and then **returns**. Every
piece of post-store bookkeeping the plain `operatorText == "="` path runs afterwards is therefore
skipped for `??=`:

- `TransferPointerOwnershipOnStore`
- `TransferMoveStringOwnershipOnStore`
- `MarkVariableUnmoved` / `MarkVariableFieldUnmoved` / `MarkVariableNotExplicitlyMovedNull`
- `ClearOwnMoveOrigin` (the `--sanitize=ownership` origin slot)
- `ClearVariableBond`
- `SetVariableBorrowsOwnedString` (the string field-borrow refresh)
- `SetVariableBorrowsOwnedElement` (the container-element borrow refresh)

The last of these was the one that bit: an element borrow assigned through `??=` kept a DECLARATION
fact that the store may have invalidated. **All seven calls are still skipped.** The interface-box
guard does NOT re-run the element refresh in the `??=` branch; it instead neutralizes the possibly
stale element fact for the BOXING proof only, via the `CoalesceRebound` flag set by
`MarkPointerRebound` (clearing the taint outright was measured widening the RAW-delete guard, a
behaviour change master does not have). The raw-delete guard still reads `BorrowsOwnedElement`
directly, exactly as master does.

## Why it was not fixed there

Each of the remaining calls is a separate ownership subsystem with its own accept-set (moves, bonds,
owned strings, the sanitizer's origin tracking). Routing `??=` through the shared tail is the right
long-term shape, but it changes behaviour for move/bond/string programs that have nothing to do with
interface boxing, and none of them had a failing repro in hand. Fixing them under an interface-boxing
commit would have been an unmeasured widening.

## Expected symptoms

Anything whose correctness depends on a store being observed. Predicted, NOT yet reduced to repros:

- `x ??= move y;` should mark `y` moved-from and does not.
- `s ??= b.name;` should mark `s` as borrowing an owned string field and does not.
- A bonded variable reassigned with `??=` should break the bond and does not.

## MEASURED, memory-unsafe (added 2026-08-05 by `fix/tempuniq`)

The first repro of this issue that is not a prediction, and it is memory-unsafe in TWO different
ways depending on the spelling. `Res` has a destructor that counts; identical on `14097e1` and on
the merged `fix/tempuniq` (`scratch/tu/ca.cb`):

```cflat
Box<unique Res*> makeBox() { Box<unique Res*> b = default; b.t = new Res(); b.t->v = 70; return b; }

Res* raw = nullptr;  raw ??= makeBox().t;                    // v=70,         dtors=0  -> LEAK
Res* r2  = nullptr;  r2  ??= c > 0 ? makeBox().t : nullptr;  // v=1431655765, dtors=1  -> UAF
```

The bare spelling never registers the owning temp at all, so nothing is ever freed; the join
spelling does register it, frees at end of statement, and leaves `r2` dangling. Both slip the
temp-unique-field escape gate for the same reason the seven calls above are skipped: the `??=`
branch RETURNS before the shared store tail, so it is not one of the persist sites, and adding it
to the list of guarded sites is not the fix - routing `??=` through the tail is.

Deliberately NOT closed by `fix/tempuniq`: that branch's accept set is the seven unrelated
ownership subsystems above, which is a wider change than the escape gate this issue is being
noted from. This raises the issue's severity evidence but it stays P2 under the
residue-not-regression precedent; re-rank if the memory-unsafe-accept rubric wins.

## Fix direction

Hoist the common post-store tail out of the `=` branch into a helper and call it from both, or make
the `??=` handler fall through to the shared tail with the store already performed. Whichever shape,
the accept-set has to be built per subsystem first - the `??=` path currently gets its *lack* of
bookkeeping baked into whatever passes today.

## MEASURED, memory-unsafe (added 2026-08-06 by `fix/assign-gate`)

Another concrete repro of the same root cause, this time bypassing a PROVENANCE gate rather than
ownership bookkeeping. `fix/assign-gate` added `CheckThinFnPtrAssignProvenance` to the plain `=`
path (local, field, generic-encoded field, decl-init, brace field-init) so a data pointer can no
longer be assigned into a thin `function<>` destination. The `??=` handler builds its own
compare/branch/store and returns before that gate ever runs (same shape as every other skipped
check this file documents), so it stayed a live hole:

```cflat
function<int(int)> f = default;
void* vp = &q;
f ??= vp;   // compiles clean, then CALLS vp as code
```

Measured on `x64/Release/cflat` (worktree `fix/assign-gate`, merge-base `68c78fc` plus the new
gate): compiles exit 0, runs exit 138 (SIGBUS) - the exact same defect shape the just-landed
assignment gate closes for `=`, decl-init, and brace-init. The FAT `Lambda<>` twin
(`g ??= vp;`) is NOT an open hole by the same mechanism: it fails for an unrelated reason first -
`??=`'s null-check condition (`if (x == 0) x = rhs;`) requires a scalar, and a fat closure is a
16-byte `{code, env}` struct, so `condition must be a scalar ... not '__closure_fat_ptr'` fires
before the store is ever reached. Only the THIN `function<>` destination is live.

Not fixed here: the gate reads a `NamedVariable` (declared-pointer-shape evidence), and the
`??=` handler evaluates its RHS through the lean `ParseAssignmentExpression(assignCtx)` VALUE path
specifically because it discards that NamedVariable (see the "Code-value store gate for `??=`"
comment on the opposite-direction gate, same handler) - a bare `void*` LOCAL read has no PHI/join
for `JoinDeliversDataValue` to inspect and no declared-type carrier to consult, so a naively
reconstructed empty-flags `NamedVariable` would silently NOT reject it (false accept, worse than
today's known gap). Closing this requires either running `ParseAssignmentExpressionNamed` in the
`??=` branch (thereby also picking up the seven skipped bookkeeping calls above, which is a wider
change) or introducing a value-level "provably data" ledger query independent of NamedVariable.
Left open; scope stays whatever fixes the general `??=`-skips-the-shared-tail defect.

## Related

[[interface-boxing-keyed-on-source-binding]] - the guard whose `??=` leg forced the element fact
to be neutralized for the boxing proof (`CoalesceRebound`).
