# `unique <interface>` move: readable-as-null, and cross-block deref diagnosis

Status: DESIGN, not started. Promoted from the residual section of
`internal/issue/deref-of-moved-pointer-guard-inside-callee.md` because closing it is a
LANGUAGE semantics change to what a moved-from interface local is, not the one-line
`RecordNullDerefFor` call that residual advertised.

## The gap

A conditional or loop-carried move of a `unique <interface>` local followed by a dispatch is
not diagnosed, so it segfaults at runtime if the moving branch is ever taken:

```cflat
unique IBox ig = new BoxImpl();
if (RuntimeFalse()) { unique IBox ig2 = move ig; }
int t = ig.tag();          // NOT diagnosed; segfaults when the branch is taken
```

The equivalent thin-pointer program IS diagnosed by the cross-block MAY-null fixpoint
(`nulldf`, `cflat/MoveDataflow.h`). Same-block interface deref is already rejected
(`Test/errors/err_move.cb`, the `ui` leg) - the gap is strictly cross-block.

## Why it is not a one-line fix

The interface deref site (`cflat/MainListener.h:16577-16586`, the
`interfaceVar.TypeAndValue.IsInterface` arm of member access) deliberately records NO `Deref`
event and checks only the same-block `IsExplicitlyMovedNullHere`. Adding
`RecordNullDerefFor(interfaceVar, ...)` there would make the fixpoint fire - and the
programmer would then have NO WAY TO SILENCE IT, because the guard the thin-pointer form uses
is itself rejected on an interface:

```cflat
unique IB ig = new BImpl();
if (RuntimeFalse()) { unique IB ig2 = move ig; }
if (ig != nullptr) { t = ig.tag(); }    // error: use of moved variable 'ig'
```

(Verified against `x64/Release/cflat` at the time of writing. Note `ig != nullptr` compiles
and works fine on a NON-moved interface local, so the fat-pointer null comparison itself is
already implemented - the rejection is purely the move state.)

The asymmetry is in the two explicit-move paths:

| | thin `unique R*` local | `unique <interface>` local |
|---|---|---|
| site | `cflat/MainListener.h:15398-15401` | `cflat/MainListener.h:15307-15317` |
| zero the slot | yes (null store) | yes (`ConstantAggregateZero` fat ptr) |
| `MarkVariableExplicitlyMovedNull` | yes | yes |
| `MarkVariableMoved` (sets `IsMoved`) | **no** | **yes** |

`IsMoved` is consulted by `MovedUseSubject` (`cflat/LLVMBackend.h:16243`) on EVERY value read
via `LoadNamedVariable` (`cflat/MainListener.h:12386`), so a moved interface local is fully
poisoned: no comparison, no pass-along, no narrowing. A moved thin pointer is nulled but
plain-readable by design (see `[[explicit-move-nulls-source]]`; `Test/test_move.cb`'s
`explicit_move_local_*` legs), which is exactly what makes a repair guard expressible.

## The change

Bring `unique <interface>` locals onto the thin-pointer contract - **moved means nulled and
plain-readable, dereference-rejected** - then turn the diagnostic on.

1. **Drop `MarkVariableMoved` from the interface explicit-move path**
   (`cflat/MainListener.h:15315`), keeping the zero-store, `MarkVariableExplicitlyMovedNull`,
   and `lastOwningResult`. This is the semantics change: reads of a moved interface local
   become legal and observe a zeroed fat pointer.
2. **Confirm the zeroed fat pointer compares equal to `nullptr`.** The comparison lowers on
   the data pointer; a `ConstantAggregateZero` fat value has a null data pointer, so
   `ig == nullptr` should already be true. Verify, and if the comparison reads the vtable slot
   instead, fix it to test the data pointer.
3. **Record the `Deref` event at the interface dispatch site**
   (`cflat/MainListener.h:16577-16586`): call `RecordNullDerefFor(interfaceVar, line, col)`,
   skipping `?.` exactly as the `->`/`.` site does. `RecordNullDerefFor`
   (`cflat/LLVMBackend.h:16198`) already accepts a non-pointer owning/unique local, and
   `nulldf` is name-keyed, so no dataflow change is needed.
4. **Method-call receivers** dispatch through the same arm, so `ig.tag()` is covered by (3).
   Interface FIELD access (`InterfaceFieldIndex` branch, same arm) is a dereference too and
   must record the same event.
5. **Interface arrays / `unique <interface>` struct fields** stay out of scope, matching the
   thin-pointer diagnostic's whole-local-only rule (`RecordNullDerefFor` bails on a non-empty
   `FieldName`).

## Test fallout - the part that needs a decision

- `Test/test_move.cb`, `cross_block_conditional_move_then_deref_interface` (line ~837)
  asserts that the unguarded program above **compiles clean and returns 42**. Closing this
  gap INVERTS that leg's intent. Rewrite it as the guarded form (`if (ig != nullptr)`,
  expected `-1`/sentinel, mirroring `cross_block_conditional_move_then_deref` two legs above),
  and move the unguarded form to `Test/errors/err_move.cb` as an
  `expect_error("dereference of moved variable 'ig'")` leg. The current leg's comment
  explicitly justifies itself by "an interface local has no post-move null-check escape
  hatch" - step 1 removes that premise, so the comment must be rewritten too.
- `Test/errors/err_move.cb`'s same-block `ui` leg keeps passing unchanged (same-block deref
  still errors via `IsExplicitlyMovedNullHere`).
- Any leg asserting `use of moved variable` on an INTERFACE local flips to legal. Grep
  `err_move.cb` and `test_move.cb` for interface legs before starting; at the time of writing
  the only interface legs are the two above.
- `cross_block_legs_no_leak` (dtorCount) must still hold: step 1 does not change ownership
  transfer, only the read-poison bit, so the destructor accounting is untouched. Verify it
  did not move.

## Risks

- **False positives are the failure mode to watch.** The thin-pointer diagnostic's
  false-positive-freedom rests on control dependence plus the read-kill, both name-keyed and
  shape-agnostic; an interface local feeds the identical machinery, so the argument carries
  over unchanged. The boundary statement in the issue file applies verbatim.
- **Widening reads of a moved interface local is the real blast radius**, not the diagnostic.
  Code that today gets a clean `use of moved variable` error will instead compile and observe
  a zeroed fat pointer. That is precisely the thin-pointer bargain already shipped, but it
  means a program that dispatches on a moved interface under a guard the analysis cannot see
  now traps at runtime rather than at compile time. Acceptable only because the same trade is
  already live for `unique R*`.
- Fat-pointer zeroing must be complete (both slots), or a stale vtable pointer survives and a
  dispatch on a "null" interface jumps into freed memory instead of faulting cleanly.

## Verification

`./cmake_build.sh release && ./test.sh Release` - the bar is the full suite green, with the
two rewritten `test_move.cb` / `err_move.cb` legs proven RED by reverting step 3.
