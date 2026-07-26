# `unique <interface>` move: readable-as-null, and cross-block deref diagnosis

Status: IMPLEMENTED (2026-07-26), landed as commit bf26a15 "Implement Unique interface".
All five implementation-order steps below are done and the macOS suite is green (482/0/8,
test.sh Release), with the diagnostic proven load-bearing by a red-proof. Remaining follow-up
work discovered during implementation is tracked in its own issue files, NOT here:
`internal/issue/ternary-owning-struct-borrow-arm-double-free.md` (struct-join double free,
found by the step-5 audit) and `internal/issue/ternary-iface-borrow-arm-module-verify.md`
(borrowed thin-pointer arm in an interface ternary fails module verification). This file is
kept as the design record. Promoted from the residual section of
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

The interface deref site (`cflat/MainListener.h:16654-16668`, the
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
| site | `cflat/MainListener.h:15463-15484` | `cflat/MainListener.h:15377-15395` |
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
   (`cflat/MainListener.h:15384`), keeping the zero-store, `MarkVariableExplicitlyMovedNull`,
   and `lastOwningResult`. This is the semantics change: reads of a moved interface local
   become legal and observe a zeroed fat pointer.
2. **[RESOLVED - no change needed] The zeroed fat pointer compares equal to `nullptr`.**
   Verified: the equality path normalizes an interface-vs-null compare by extracting the
   DATA slot (`{1u}`, `cflat/MainListener.h:11408-11411`; fat layout is `{vtable, data}`),
   and the move path stores `ConstantAggregateZero`, which zeroes BOTH slots - so
   `ig == nullptr` is true after a move and no stale vtable survives (this also discharges
   the last risk bullet below).
3. **Record the `Deref` event at the interface dispatch site**
   (`cflat/MainListener.h:16654-16668`): call `RecordNullDerefFor(interfaceVar, line, col)`,
   skipping `?.` exactly as the `->`/`.` site does. `RecordNullDerefFor`
   (`cflat/LLVMBackend.h:16198`) already accepts a non-pointer owning/unique local, and
   `nulldf` is name-keyed, so no dataflow change is needed.
4. **Method-call receivers** dispatch through the same arm, so `ig.tag()` is covered by (3).
   Interface FIELD access (`InterfaceFieldIndex` branch, same arm) is a dereference too and
   must record the same event.
5. **Interface arrays / `unique <interface>` struct fields** stay out of scope, matching the
   thin-pointer diagnostic's whole-local-only rule (`RecordNullDerefFor` bails on a non-empty
   `FieldName`).

## Test fallout - DECIDED as follows

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

## Fix design (finalized 2026-07-26)

Extend the strict all-arms rule to interface fat pointers: an arm joins as owning only when
it is an owning temp (owning-return ledger or owning-`new` ledger), a moved-out value, or a
null/zeroinitializer constant; a mixed join suppresses ADOPTION as well as release, keeping
the entry visible to the no-discard check exactly as the pointer path does
(`CallerReleaseSuppressed`, `ClearOwnedResultChannels`).

Verified structure of the join paths (this shapes the implementation):

- `PropagateTernaryOwnership` already runs on EVERY ternary join - phi at
  `cflat/MainListener.h:11103`, select at `:11219` - regardless of type. Only the
  `mixedPtrJoin` gate inside it (`cflat/LLVMBackend.h:2225`) is `isPointerTy()`-only; the
  either-arm ledger stamping above that gate is what launders ownership today (the comment
  at `:2219-2220` documents the either-arm rule for non-pointer joins as the then-status quo
  and must be rewritten with this change).
- There are TWO interface join shapes, and they see different arm values:
  1. **Thin-arm phi, upcast to fat** (`UpcastTernaryPhiToInterface`,
     `cflat/MainListener.h:9523`): the ORIGINAL phi's incoming values are the raw thin
     pointers (the `new` result, the borrowed load) - exactly the values the existing
     pointer ledgers already track. Run `TernaryArmJoinsOwning` on
     `phi->getIncomingValue(i)` per arm (a null arm stays joinable) and carry the verdict
     onto the new fat phi: mixed => `SuppressCallerRelease(fatPhi)` +
     `ClearOwnedResultChannels()`; all-owning => propagate the ledger entries onto the fat
     phi exactly as `PropagateTernaryOwnership` does for the thin one.
  2. **Already-fat arms** (both arms interface-typed, e.g. `cond ? makeShapeMove() :
     borrowed`): the phi is fat-typed from the start and `PropagateTernaryOwnership` sees
     fat STRUCT values. The owning-return ledger already holds fat call results by value
     identity (that is what stamps the join today), so the fix is widening the
     `mixedPtrJoin` gate to `isPointerTy() || type == GetFatPtrType()`, with
     `TernaryArmJoinsOwning` unchanged - it is value-identity-based and
     `Constant::isNullValue()` already covers `ConstantAggregateZero`. Caveat:
     `GetFatPtrType()` is shared with closure/lambda fat values, so widening the gate pulls
     closure joins under the strict rule too - audit that this is sound (a mixed closure
     join suppressing is safe; wrongly freeing one is not) before shipping.
- **The `move`-arm gap is real, and it is the Part 1 coupling**: the interface
  explicit-move path (`cflat/MainListener.h:15377-15395`) sets only the sticky
  `lastOwningResult` - it never ledgers the moved-out fat value, unlike the thin path's
  `RegisterMovedOutPtrValue(ptrVal)` at `:15489`. Add the same call for the captured fat
  value (`movedOutPtrValues_` stores `llvm::Value*`, so fat values fit as-is). The
  borrowed-source gate ba1b886 added on the thin path is supplied here by the existing
  `IsVariableOwning(CallerName)` condition guarding the whole branch - a `move` of a
  borrowed interface never enters it, so it can never ledger as owning.

DECIDED policies (mirroring the pointer path, commits c873f15 / c315ae0 / ba1b886):

- A mixed join into a `unique <interface>` (or owning) receiver is a COMPILE ERROR, using
  ba1b886's "cannot initialize unique ... from a borrowed value" wording - not a silent
  borrow. A mixed join observed by a plain (borrow) local is a suppressed borrow; the
  allocating arm leaks, and the no-discard diagnostic still fires on a discarded join.
- A leak on the mixed join is the accepted trade, exactly as on the pointer path; a
  use-after-free is not.

Test fallout - RE-ASSESSED against the current tree, milder than originally feared:

- `Test/test_move.cb` `testUniqueInterfaceTernary` (line 1873): the earlier claim that its
  13 assertions pin either-arm adoption of MIXED joins is WRONG for the current tree.
  Every leg is owning/null, owning/owning (`new SqMove() : new CiMove()`), or null/null -
  all satisfy the strict all-arms rule and keep passing unchanged. No rework needed.
- No existing leg covers a genuinely mixed (borrow-arm) interface join. Add two: the UAF
  repro above as an `expect_error` leg in `Test/errors/err_move.cb` (unique receiver =>
  compile error), and a plain-borrow-receiver leg in `testUniqueInterfaceTernary`
  asserting no adoption (dtorCount stays 0 inside and after the inner scope, owner still
  dispatchable - the accepted leak is not observable via dtorCount).
- The other owning-value struct joins (`string`, owning-value structs, closure fat
  pointers beyond the shared-type caveat above) reach the same either-arm stamping and
  should be AUDITED for the same shape, but fixing them is OUT OF SCOPE here - their
  release paths differ (`FlushOwnedStringTemps` / `FlushOwnedStructTemps`) and each needs
  its own per-arm owning test. File one `internal/issue/` entry per confirmed repro.

## Implementation order

1. [DONE] Part 2, already-fat gate: widen `mixedPtrJoin` to the fat-pointer type, add the
   mixed-join-into-unique compile error, add the two test legs. This alone closes the UAF.
2. [DONE] Part 2, thin-arm upcast: per-arm verdict around `UpcastTernaryPhiToInterface`.
3. [DONE] Part 2, move-arm ledgering: `RegisterMovedOutPtrValue` of the captured fat value in the
   interface explicit-move path.
4. [DONE] Part 1 steps 1, 3, 4 (drop `MarkVariableMoved`; record `Deref` events at dispatch and
   interface-field sites), then the Part 1 test rewrites. Step 2 needs no code.
5. [DONE 2026-07-26] The string/struct/closure join audit, filing issues only. Verdicts:
   string SOUND (every ternary string arm is deep-copied before the phi); closures untouched
   (separate __closure_fat_ptr type); owning-value STRUCT join is a confirmed double free -
   filed as internal/issue/ternary-owning-struct-borrow-arm-double-free.md (compound of the
   non-strict struct join AND a decl-init gap that shallow-copies a ternary phi).
