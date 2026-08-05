# Interface-field-to-interface-field `unique` copy misread as a self-assign, no diagnostic

Found while probing `unique-field-to-field-residue-temp-and-interface-source` (round-2
adversarial review of `fix/unique-f2f`). **Pre-existing** - verified identical on the pre-fix
binary (`3b6e3e8`) and on the fix commit, so it is neither caused nor worsened by that change.
It is a distinct defect (a false-negative in the self-assign discriminator), not a gate-width
problem, so it gets its own file rather than a bullet in the residue file.

Severity: silent abort (exit 134), no diagnostic at all - same class as the field-to-field issue
this fix closed, but reached through a different bug.

## Repro

```cflat
struct Payload { int v = default; };
interface ISlot { unique Payload* slot; };
class BoxA : ISlot { unique Payload* slot = nullptr; };
extern int main()
{
    BoxA a = default; a.slot = new Payload(); a.slot->v = 12;
    BoxA c = default;
    ISlot ia = a;  ISlot ic = c;
    ic.slot = ia.slot;
    printf("s12 %d\n", ic.slot->v);
    return 0;
}
```

Measured on both the pre-fix binary and the fix commit, identical:
```
s12 12
```
exit 134 (compile rc 0, run rc 134) on both. `ia` and `ic` are two DIFFERENT boxed interface
values (different underlying `BoxA` instances), yet the store compiles as if it were a no-op
self-assign. (Same stability caveat as the residue file: this is a use-after-free read: treat
"compiles, then aborts with no diagnostic" as the signature, not the exact printed number.)

## Root cause - confirmed by direct source trace, not by description

An earlier draft of this file was going to cite a `CallerName`-compares-equal account relayed in
review chat. Per instruction, that account was independently re-derived from the source below
before being written down here - it holds, but on its own merits, not on the earlier framing's.

`ParseAssignmentExpression` computes `selfFieldAssign` up front
(`cflat/MainListener.h:12175-12177`, current line numbers on `fix/unique-f2f`):

```cpp
bool selfFieldAssign = !namedVar.FieldName.empty()
    && namedVar.FieldName == rightNV.FieldName
    && namedVar.CallerName == rightNV.CallerName;
```

There is a SEPARATE, correctly-guarded disjunct two lines below
(`cflat/MainListener.h:12184-12187`, `selfUniqueFieldAssign`'s second arm) for the bare
`item = item` self-field case inside a method; that arm requires
`!namedVar.TypeAndValue.VariableName.empty()` and is not implicated here - it cannot produce an
empty-vs-empty match. The actual culprit is `selfFieldAssign` itself, above it.

The interface-field member-access branch that materializes both `ia.slot` and `ic.slot`
(`cflat/MainListener.h:19342-19394`) unconditionally resets the working `NamedVariable` to
default at entry (`namedVar = {};`, line 19348) and, walking every line to the end of the branch
at 19394, never assigns `namedVar.CallerName` anywhere in between - only `Storage`, `BaseType`,
`Primary`, `TypeAndValue`, `OwningStructName`, `FieldName`, and `IsInterfaceField` are set
(19385-19394). The only `CallerName` token in the whole branch is `interfaceVar.CallerName`, a
DIFFERENT variable (the receiver being dereferenced), read only for an unrelated diagnostic
string. So `namedVar.CallerName` for any interface-field read materialized through this branch
is provably always `""` - not merely observed empty in one run, but structurally incapable of
being anything else, since no other assignment site touches it.

`ia.slot` and `ic.slot` therefore both carry `CallerName == ""` and the SAME `FieldName`
(`"slot"`) - `selfFieldAssign` reads two DIFFERENT receivers' empty caller names as proof of
sameness, and the field-to-field reject this fix's commit added
(`cflat/MainListener.h:12267-12276`, guarded by `!selfUniqueFieldAssign` at line 12271) never
fires.

**The control is airtight**: rename the fields apart so `FieldName` differs (`i2.b = i1.a`
instead of the same field name on both sides) and the SAME shape of copy (a genuinely different
interface-field receiver, boxed-interface source into a boxed-interface destination) IS
rejected on both the pre-fix and fix binaries:

```cflat
struct Payload { int v = default; };
interface ISlotA { unique Payload* a; };
interface ISlotB { unique Payload* b; };
class BoxAB : ISlotA, ISlotB { unique Payload* a = nullptr; unique Payload* b = nullptr; };
extern int main()
{
    BoxAB c1 = default; c1.a = new Payload(); c1.a->v = 12;
    BoxAB c2 = default;
    ISlotA i1 = c1;  ISlotB i2 = c2;
    i2.b = i1.a;
    printf("s12 %d\n", i2.b->v);
    return 0;
}
```
```
uf2f_iface_selfassign_control.cb(10,4): cannot store unique field 'a' into unique field
'ISlotB.b' - the source field's synthesized destructor already frees it, and two 'unique'
fields cannot own one pointer. Use 'move a' to transfer ownership out of the source field
(which nulls it).
```
compile rc 1 on both binaries - confirming `selfFieldAssign`'s two-empty-`CallerName`
false-positive, not the ownership gate itself, is what suppresses the repro above.

## How far the hole extends - checked all five `selfUniqueFieldAssign`/`selfFieldAssign` guards

`selfUniqueFieldAssign` (`:12184-12187`) is `selfFieldAssign || <a differently-guarded arm>` -
whenever `selfFieldAssign` misfires, `selfUniqueFieldAssign` is unconditionally true too, so
EVERY guard reading either flag is equally compromised IN PRINCIPLE. There are five such guards
before/around the field-to-field leg; only one is reachable for this repro's shape (a raw
`unique T*` interface field, direct copy, no ternary join). Checked each by reading its own
independent gating condition, not by running the pre-fix binary against all five (four of the
five have preconditions that cannot be simultaneously satisfied with this repro's shape, so
there is nothing to run):

1. **`RejectOwningValueCopyIntoField`** (call site `:12208-12210`, definition at `:10661`,
   gate at `:10670-10684`). Requires `right->getType()->isStructTy() && !rightNV.TypeAndValue.Pointer`.
   A `unique Payload*` field is pointer-typed, so this function returns `false` on its own type
   gate before `isSelfAssign` (the parameter carrying `selfUniqueFieldAssign`) is even consulted.
   **Not reachable for this shape** - excluded by an unrelated, correct gate, not saved by luck.

2. **`RejectNonHeapAddressIntoUnique`** (guard `:12214-12220`, `!selfUniqueFieldAssign` at
   `:12219`). Requires `LLVMBackend::IsProvableNonHeapAddress(right)`
   (`cflat/LLVMBackend.h:15282-15292`), which is true only when `right` IS an alloca/global
   address (or a GEP chain rooted at one) - i.e. the value itself is `&local`-shaped. A field
   read like `ia.slot` produces a LOADED value (a `LoadInst` through the interface's data
   pointer), never an alloca/global address. **Not reachable for this shape** - structurally
   cannot co-occur with a source that is also a matching-`FieldName` field read.

3. **`RejectBorrowIntoUniqueField`** (Trap A, guard `:12233-12237`, requires
   `rightNV.IsBorrowed`). The interface-field materialization branch traced above
   (`:19342-19394`) never assigns `IsBorrowed` on the resulting `NamedVariable` at all - grepping
   the whole branch confirms no such assignment exists. **Not reachable for ANY interface-field
   read**, not just this example; a plain interface-field access never carries borrow
   provenance today, a separate, likely pre-existing gap from this one.

4. **The `?:`-join Trap A variant** (guard `:12246-12251`, requires
   `compiler->IsMovedBorrowedPtrValue(rightNV.Primary, ...)`). That ledger
   (`movedBorrowedPtrValues_`) is populated only by the ternary-join lowering path
   (`cflat/LLVMBackend.h:2533-2534`); a plain field read never registers into it.
   **Not reachable for this shape** - requires an unrelated preceding `?:` join.

5. **`RejectUniqueFieldToUniqueField`** (the field-to-field leg this fix's commit added, guard
   `:12267-12271`). **Reachable and confirmed suppressed** - this is the repro above.

**Bottom line**: the `selfFieldAssign` defect is general (it would compromise any of the five if
their own preconditions ever lined up with an interface-field empty-`CallerName` collision), but
for the shape this repro demonstrates - and for interface-field reads in general, per point 3 -
only the field-to-field leg (5) is actually reachable today. A fix should not assume the other
four are equally exposed; they are excluded by independent, correct gates unrelated to this bug.

## Fix direction - polarity matters

Two options were on the table originally:
1. Set `CallerName` at the interface-field materialization site (`:19391-19394`) so two
   different receivers are distinguishable the same way a plain struct field access already is.
2. Make `selfFieldAssign` refuse to conclude self-assign purely from two EMPTY caller names.

Option 1 is still untried and still carries the blast radius the `fix/funcptr-callresult`
landed record describes (`CallerName` feeds `ScoreMoveAgreement`'s rvalue test, the bond-source
ledger, and move tracking).

## ATTEMPTED AND REVERTED (2026-08-01): receiver identity is NOT provable from NAMES

Option 2 was implemented on `fix/uniq-family` as an extra conjunct on `selfFieldAssign`:

```cpp
bool differentFieldReceivers =
    !namedVar.TypeAndValue.ParentVariableName.empty()
    && !rightNV.TypeAndValue.ParentVariableName.empty()
    && namedVar.TypeAndValue.ParentVariableName != rightNV.TypeAndValue.ParentVariableName;
```

It closed the repro above and **false-rejected working programs**, so it was reverted before
landing. The defect: it concludes two receivers are different objects from variable-NAME
inequality, and **two names can denote one object**. That is a rejection on an unproven
premise - strictly worse than the missing diagnostic it was fixing.

Both witnesses below compile, run, and free exactly once on master (`cf5e909`) and were
rejected outright by the attempted fix. Verified by direct measurement on both binaries.

**Witness 1 - `alias`, which is by definition the same object:**

```cflat
int gfreed = 0;
struct Node { int v = default; ~Node() { gfreed = gfreed + 1; } };
interface ISlot { unique Node* slot; };
class BoxA : ISlot { unique Node* slot = nullptr; };
extern int main()
{
    {
        BoxA a = default; a.slot = new Node(); a.slot->v = 7;
        ISlot ia = a;
        alias ISlot ib = ia;
        ib.slot = ia.slot;
        printf("v=%d freed=%d\n", ib.slot->v, gfreed);
    }
    printf("after freed=%d\n", gfreed);
    return 0;
}
```
Master: compile rc 0, prints `v=7 freed=0` then `after freed=1`, exit 0 - one owner, freed
exactly once. Attempted fix: compile rc 1,
`cannot store unique field 'slot' into unique field 'ISlot.slot'`.

**Witness 2 - two boxes of the SAME object, no `alias` involved:** replace the two receiver
lines above with `ISlot ia = a; ISlot ib = a;`. Same split, same numbers.

**What will NOT work, so the next attempt does not re-derive it:**

- **Variable names.** Both witnesses above. Do not ship a name comparison in any form.
- **The interface LOCALS' storage.** `ISlot ia = a; ISlot ib = a;` boxes `a` twice into two
  distinct allocas, so comparing the locals' `Storage` (or the fat values loaded from them, as
  LLVM Values) reports "different" for witness 2 and false-rejects it identically.
- **A bare LLVM `Value` comparison of the computed field address.** Each access re-loads the
  fat pointer, so even the TRUE self-assign `ia.slot = ia.slot` produces two distinct
  `LoadInst`s and would compare unequal - inverting the bug into a false rejection of the one
  shape master gets right.

What an actual fix needs is proof that the two field lvalues denote DIFFERENT SLOTS - i.e. the
boxed DATA pointers differ. Both witnesses share one data pointer (`&a`) while differing in
name and in local storage, so the discriminator has to reach through the box to the underlying
object. Comparing the data pointers after resolving each back to its root store is the
direction to try; note it must answer "same" for two distinct boxes of one object, which is
what makes this more than a `Value` equality test.

Keep the polarity: `selfFieldAssign` treating "cannot tell" as SAME is what makes today's bug
a missing diagnostic rather than a false rejection. Any fix must PROVE difference before
rejecting, and accept whatever it cannot prove.

## Where this sits among the three receiver kinds (2026-08-02)

The same empty-or-ambiguous `CallerName` root cause has now been measured in three receiver
kinds: an ARRAY ELEMENT (`CallerName` names the container - closed for constant indices by
`fix/uniq-array-elem`), a GLOBAL struct (`CallerName` empty - closed 2026-08-04 by
`fix/uniq-global`, which proved two distinct stack/global ROOTS apart rather than touching the
name predicate), and an interface field (this file).

They share a cause and do NOT share a remedy. The other two are reachable from the scope-stack
identity the compiler already has: `stackNamedVariable` resolves a local to a `NamedVariable`
carrying `Storage`, and `ProvablyDifferentSlots` already proves slots apart from it. **That route
is ruled out HERE by measurement, not by argument** - the "What will NOT work" list above records
that comparing the interface locals' `Storage` false-rejects witness 2 (`ISlot ia = a;
ISlot ib = a;` - one object, two boxes, two distinct allocas), and that a bare `Value` comparison
of the field address false-rejects even the true self-assign, since each access re-loads the fat
pointer.

So do not import a fix from the global or array-element work on the assumption that one
mechanism covers all three. This face still needs the boxed DATA pointer resolved back to its
root store, and it must answer "same" for two distinct boxes of one object.

## Test coverage

None, and none can be added while it reproduces. `Test/errors/err_unique_borrow_into_field.cb`
carries a NOTE pointing here in place of a leg. Two must-keep-working legs guarding the
neighbouring shapes are in `Test/test_move.cb::testUniqueFieldStoreRemedies`
(`uniq_iface_selfassign_*` - one receiver assigning its own field to itself, and
`uniq_iface_move_*` - `move` between two receivers, which does transfer and null correctly).
When this is fixed, the leg to add is the SAME-field-name shape: the different-name control
already rejects on master, so a leg spelled that way is vacuous.

Related: [[interface-issue-queue]]
