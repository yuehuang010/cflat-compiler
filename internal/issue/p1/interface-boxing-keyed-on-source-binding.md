# `delete` of a BORROWED interface box is not diagnosed (double free)

NARROWED 2026-07-31. This file was the 2026-07-30 consolidation of three files
(`interface-boxing-guards-are-binding-dependent`,
`null-coalesce-join-into-interface-not-boxed`,
`interface-boxing-sites-not-fully-consolidated`) on the root "the boxing path decides what to do
by looking up the source `NamedVariable`, so a spelling that carries no binding gets neither the
guards nor, in one case, the boxing at all."

That root is CLOSED. What is left is a DIFFERENT root that the old file's `?:` repro was blaming
on the closed one - see "Why the `?:` repro is not the same bug" and "What was closed" below.

## The live defect

Deleting an interface box the receiver only BORROWS is accepted and double-frees at runtime. It
needs no join at all - the minimal repro has no `?:` and no `??`:

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } ~Circle() {} };

int borrow(Circle* p)
{
    IShape s = p;      // p is BORROWED - the box borrows too, correctly
    delete s;          // accepted; frees an object this frame does not own
    return s.area();
}

extern int main()
{
    Circle* c = new Circle(); c.r = 2;
    int a = borrow(c);
    delete c;          // second free
    return a;
}
```

Exit 139 (use after free), no diagnostic, on the pre-fix and post-fix binaries alike.

The `?:` / `??` join spelling is the same defect reached through the join's non-adoption rule:

```cflat
Circle* a = new Circle(); a.r = 2;
Circle* b = new Circle(); b.r = 3;
IShape s = k > 0 ? a : b;   // a JOIN into a plain interface local: a BORROW by design
delete s;                   // accepted; a and b still own, and free again at scope exit
```

Exit 133/134 on both binaries.

## Why the `?:` repro is NOT "boxing is keyed on the binding"

The old file read the `?:` double free as a missing ownership TRANSFER and prescribed transferring
per arm in `UpcastTernaryPhiToInterface`. That was implemented, measured and reverted on
2026-07-31, and an independent review then rebuilt the branch WITH it enabled and measured it
again; both runs agree it trades the double free for a use-after-null. **The full account, the
mechanism, and why no guard can rescue it are the "DO NOT RETRY" paragraph of the
`fix/iface-boxing` landed design record in [[interface-issue-queue]]** - that is the durable home,
because this file is deleted when its (now different) bug is fixed. The short version: a join into
a plain interface local is a BORROW by design, `Test/test_move.cb`'s
`iface_ternary_thin_borrow_arm_*` legs pin it (nulling the owning arm exits 139 at
`owner->area()`, `test.sh` 539/1), and the consume-vs-borrow fact lives on the DESTINATION, not on
the arm.

So the join behaves as designed; the unchecked `delete` is the defect, and it is not
join-specific.

## Fix direction

Reject `delete <interface value>` when the value is PROVABLY a box the frame does not own. The
provenance ledger `LLVMBackend::interfaceBoxRecords_` already records `Source` and
`OwnershipTransferred` per box, and `SuppressCallerRelease` already marks a non-adopted join, so
the facts exist. Polarity is the whole difficulty: reject only a box PROVEN borrowed (a ledgered
record with `OwnershipTransferred == false` and a `Parameter` / non-owning `Source`, or a join the
boxing site suppressed) and accept every value whose provenance cannot be resolved - a
move-returning call, an `IShape` parameter, a field read, a re-boxed value. A `delete` that cannot
be proven wrong must keep compiling.

## What was closed (2026-07-31)

- **Binding-erased SINGLE-VALUE sources.** `IShape s = (c);`, `IShape s = (c) as IShape;` and the
  assignment-statement `s = (c);` skipped the ownership transfer because it keyed off the source
  `NamedVariable`, which parentheses erase; both the box and `c` then owned the object and it was
  freed twice. `BoxConcreteIntoInterface` now falls back to a VALUE-keyed retirement
  (`RetireOwningSourceOfBoxedValue`): a pointer `LoadInst` off a live owning binding's slot is
  nulled and marked moved, exactly as the plain `IShape s = c;` spelling does. That MOVE is a
  RATIFIED behaviour change - `IShape s1 = (c); IShape s2 = (c);` compiled and ran before and is
  now `use of moved variable 'c'`; see the landed design record. Regression legs:
  `Test/test_move.cb` `iface_paren_box_*`, `iface_paren_as_box_*`, `iface_paren_assign_box_*`.
- **`??` into an interface was never boxed at all** (LLVM module-verifier dump, no source
  location). `??` joins through a SLOT, so its result is a plain load and its arms cannot be
  recovered from the IR; the lowering now ledgers them (`RegisterNullCoalesceJoin`) and
  `UpcastNullCoalesceToInterface` boxes per arm through the same `BoxInterfaceJoinArms` core `?:`
  uses. An unresolvable arm now gets the `?:` wording with `??` named as the operator. Legs:
  `Test/test_move.cb` `iface_nullcoalesce_left_arm` / `_right_arm`,
  `Test/errors/err_nullcoalesce_iface_arm_unresolved.cb`. Only the DECL-INIT and ASSIGNMENT
  spellings were wired up; the RETURN and CALL-ARGUMENT spellings are still broken and are filed
  as [[nullcoalesce-join-not-boxed-on-return-and-call-arg]].
- **The two open-coded boxing sites** (the assignment STATEMENT on the `=` path and
  `CoerceInitValueToInterface` for brace / element init) are routed through
  `BoxConcreteIntoInterface`, which is now the only place a single concrete source is boxed. Sites
  whose destination runs its own ownership bookkeeping (a FIELD store refcounts an escaping `new`
  rather than nulling the source) pass `adoptsOwnership = false`, so routing them changed no
  behaviour.

## Still open, preventive (was "the second sharp edge, NARROWED")

`RegisterInterfaceBox` still dedupes on `FatValue` only, so two records sharing BOTH a
`DataPointer` and the same `Source` resolve first-registered-wins. Harmless today: such a pair is
either same-`Source` or impossible (a second box off an owning binding is a hard `use of moved
variable`). Closing it properly means keying the dedupe on `(FatValue, DataPointer, Source)`.

## Related

[[return-dangle-missed-when-slot-has-extra-user]] - its residue wants the same provenance-based
reasoning as the fix direction above. [[nullcoalesce-join-not-boxed-on-return-and-call-arg]] -
the unfinished half of the `??` work. [[interface-issue-queue]]
