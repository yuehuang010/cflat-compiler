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

## Fix direction

Hoist the common post-store tail out of the `=` branch into a helper and call it from both, or make
the `??=` handler fall through to the shared tail with the store already performed. Whichever shape,
the accept-set has to be built per subsystem first - the `??=` path currently gets its *lack* of
bookkeeping baked into whatever passes today.

## Related

[[interface-boxing-keyed-on-source-binding]] - the guard whose `??=` leg forced the element fact
to be neutralized for the boxing proof (`CoalesceRebound`).
