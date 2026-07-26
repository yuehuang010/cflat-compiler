# `unique <interface>` move: readable-as-null, and cross-block deref diagnosis

Status: DESIGN, not started. Promoted from the residual section of
`internal/issue/deref-of-moved-pointer-guard-inside-callee.md` because closing it is a
LANGUAGE semantics change to what a moved-from interface local is, not the one-line
`RecordNullDerefFor` call that residual advertised.

This plan also absorbs `internal/issue/mixed-ternary-interface-fat-ptr-uaf.md` (folded in
2026-07-25, issue file deleted) - see "Part 2" at the bottom. The two items share the
interface-ownership surface and the same test files; land Part 2 first or together, since
Part 1's diagnostic work builds on the same explicit-move path Part 2's ledger gating touches.

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

---

# Part 2: strict ownership join for the interface `?:` (mixed fat-pointer UAF)

Absorbed from `internal/issue/mixed-ternary-interface-fat-ptr-uaf.md` (filed 2026-07-25).
Harm is a USE-AFTER-FREE plus a double free, not a leak.

## Symptom

A `?:` whose arms mix an owning value and a live BORROW propagates the owning bit onto the
joined value regardless of which arm the condition selects. When the BORROW arm is taken, a
`unique` local adopts a fat pointer someone else owns, destroys it at scope exit, and the
real owner then destroys it again.

```cflat
interface IShapeMove { int area(); };
int dtorCount = 0;
class SqMove : IShapeMove
{
    int s = 3;
    SqMove() { }
    int area() { return s * s; }
    ~SqMove() { dtorCount = dtorCount + 1; }
};
bool identityBool(bool b) { return b; }
move IShapeMove makeShapeMove() { return new SqMove(); }

extern int main()
{
    unique IShapeMove owner = new SqMove();
    IShapeMove borrowed = owner;
    {
        unique IShapeMove k = identityBool(false) ? makeShapeMove() : borrowed;
        printf("area=%d\n", k.area());
    }
    printf("after inner dtorCount=%d (expect 0)\n", dtorCount);
    printf("owner still alive area=%d\n", owner.area());
    return 0;
}
```

Observed: `dtorCount=1` after the inner scope (the borrow was destroyed), garbage from
`owner.area()`, then SIGABRT (exit 134) when `owner` is destroyed a second time.

## Root cause

`PropagateTernaryOwnership` (`cflat/LLVMBackend.h`) applies the strict "every arm must be
owning or null" rule only when the joined value `isPointerTy()`. An interface value is a
`{i8*,i8*}` fat pointer - a STRUCT type - so it takes the older either-arm branch: whichever
arm carries an owning-return ledger entry stamps the join, and the entry then drives adoption
at `unique IShapeMove k = ...` and the scope-exit release in `EmitOwningInterfaceCleanup`.

This is the same unsoundness the raw-pointer path had before the strict rule; the pointer
side was closed by commits c873f15 ("Stop a mixed '?:' pointer join from laundering
ownership into its receiver") and c315ae0 ("Judge a pointer binding's ownership by value
identity, not by a sticky flag"). Those commits are the template: value-identity ledgers
(`ownedNewTemps_`, owning-return ledger, `movedOutPtrValues_`), `TernaryArmJoinsOwning` as
the single join predicate, `ClearOwnedResultChannels()` on a mixed join, and
`CallerReleaseSuppressed` keeping suppressed entries visible to the no-discard diagnostic.

## Fix direction

Extend the strict all-arms rule to interface fat pointers: treat an arm as joinable only when
it is an owning temp (owning-return ledger or owning-`new` ledger) or a
null/zeroinitializer constant, and suppress ADOPTION as well as release for a mixed join,
keeping the entry visible to the no-discard check exactly as the pointer path now does
(`CallerReleaseSuppressed`). With the pointer-side machinery landed, the work is mostly
extending the `isPointerTy()` gate in `PropagateTernaryOwnership` / `TernaryArmJoinsOwning`
to recognize the fat-pointer struct type, plus per-arm boxing considerations
(`UpcastTernaryPhiToInterface` boxes each arm in its own branch already).

Things to plan for:

- `Test/test_move.cb`'s `testUniqueInterfaceTernary` pins the current either-arm adoption
  behaviour across 13 assertions. Narrowing the rule requires REWORKING those expectations
  (the mixed cases become "not adopted, the allocating arm leaks" - or a
  borrow-into-unique compile error, matching what c873f15 chose for `unique T*`), not
  reverting the rule. Do not weaken the rule to keep the old counts.
- The `move` arm of an interface ternary needs the same value-identity treatment the pointer
  side got via `movedOutPtrValues_`, including the `IsBorrowed` gate from c315ae0 - a
  `move` of a borrowed interface must not ledger as owning.
- The other owning-value struct joins (`string`, owning-value structs, closure fat pointers)
  reach the same either-arm branch and should be audited for the same shape at the same
  time. Their release paths differ (`FlushOwnedStringTemps` / `FlushOwnedStructTemps`), so
  each needs its own "is this arm actually owning" test rather than a shared
  pointer-only gate.

A leak on the mixed join is the accepted trade, exactly as on the pointer path; a
use-after-free is not. For a `unique` receiver, prefer the pointer path's precedent: a mixed
join into `unique` is a compile error ("cannot initialize unique ... from a borrowed
value"), not a silent borrow.
