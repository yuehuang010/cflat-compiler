# Interface boxing is keyed on the source BINDING, so any spelling that erases the name falls through

Consolidated 2026-07-30 from three files, all filed 2026-07-29 by reviews of the boxing
consolidation (`BoxConcreteIntoInterface`): `interface-boxing-guards-are-binding-dependent`,
`null-coalesce-join-into-interface-not-boxed`, and
`interface-boxing-sites-not-fully-consolidated`.

They are merged on the root the queue's own "structural theme" section already names: **the
boxing path decides what to do by looking up the source `NamedVariable`, so a spelling that
carries no binding gets neither the guards nor, in one case, the boxing at all.** The three
files were kept apart because their SYMPTOMS differ (double free / verifier failure /
no live defect), and the `??` file explicitly argued it was "not covered by" the guards
file - correct about the symptom, and it names the same fall-through mechanism in its own
root-cause section.

All shapes below are PRE-EXISTING and NOT regressions: identical on the pre-change binary.
The consolidation closed the gap for a NAMED source; these are the shapes that arrive with
no name.

Severity, by shape:

| Shape | Symptom |
|---|---|
| Parens, `?:` join (ownership) | **DOUBLE FREE**, exit 134 on macOS without asan. Silent in a build whose allocator does not check. |
| `??` join (decl-init) | Hard compile failure, NO source diagnostic - only an LLVM module-verifier dump naming an unnamed value. A FALSE REJECTION in effect. |
| Two un-routed boxing sites | No behavioural defect today. Consolidation debt - the invariant that made the whole family possible is still not enforced. |

## Shape 1 - parens and `?:` defeat the ownership guards (double free)

Both spellings, both freeing twice:

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } ~Circle() {} };

extern int main()
{
    Circle* c = new Circle(); c.r = 2;
    IShape s = (c) as IShape;     // parens: exit 134
    printf("paren=%d\n", s.area());
    delete s;
    return 0;
}
```

`IShape s = (c);` - the plain spelling with the same parentheses - is exit 134 too.
Removing the parentheses fixes both: `IShape s = c as IShape;` and `IShape s = c;` are
exit 0, because the source is then a named binding and the transfer runs.

A `?:` join of two owning arms is the same failure with no parentheses on the operator:

```cflat
Circle* a = new Circle(); a.r = 2;
Circle* b = new Circle(); b.r = 3;
IShape s = c > 0 ? (a) : (b);     // exit 134
delete s;
```

**Root cause.** The whole guard family keys off the source `NamedVariable` (`srcNV` in
`BoxConcreteIntoInterface`, `MainListener.h:9969`). The ownership transfer - store null into
the source storage, `MarkVariableMoved`, `MarkVariableMovedIntoInterface` - can only run when
there IS a binding to null. `SoleCastOperandOf` recovers the binding only through a pure
single-child passthrough chain down to `castExpression`. A parenthesized operand is a primary
expression wrapping a full expression, so the walk does not reach the cast level and returns
null; a `?:` join has two sources and no single binding at all. In both cases the box is
built correctly but the source keeps its owning flag, so `delete <iface>` plus the source's
scope-exit free frees twice.

## Shape 2 - a `??` join is never boxed at all (verifier failure)

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = default; int area() { return r * r; } };

extern int main()
{
    Circle* maybe = nullptr;
    Circle* p = new Circle(); p->r = 9;
    IShape s = maybe ?? p;       // no diagnostic; module verification fails
    printf("%d\n", s.area());
    return 0;
}
```

Both binaries, `-o` and `--check`, exit 1 with:

```
Module verification failed:
Invalid bitcast
  %10 = bitcast ptr %9 to %__iface_fat_ptr
```

`IShape s = p;` and the `?:` spelling `IShape s = c ? p : q;` both work, so the trigger is
`??` specifically.

**Root cause, not fully diagnosed.** Same fall-through, one step earlier: the decl-init
interface path needs a `TypeName` on the RHS `NamedVariable` to pick a boxing branch, and the
`??` join result carries none. The `?:` join has a dedicated recovery
(`UpcastTernaryPhiToInterface`, reached when `structName` is empty); `??` produces a
select/phi that either is not a `PHINode` or is not routed to that helper, so every branch of
the chain is skipped and the raw `ptr` is stored into the fat slot. This is the same
mechanism as the primitive-array bug closed on 2026-07-29 - only the reason the `TypeName` is
missing differs.

Distinct from shape 1: there the box is BUILT and only the ownership bookkeeping is skipped
(runtime double free); here the boxing never happens (compile-time verifier failure). A fix
for one does not imply the other, which is why both repros are kept.

## Shape 3 - two boxing sites are still open-coded (no live defect)

`BoxConcreteIntoInterface` (`MainListener.h:9969`) is the shared boxing site for two of the
four paths - the declaration-initializer path and `GenerateSafeCast` (the `as` path). Still
open-coded, each repeating implements-check -> `RejectPointerShapedInterfaceUpcast` ->
data-pointer selection -> `BuildInterfaceFatValue`:

- `MainListener.h:10672-10690` - the assignment-STATEMENT path (`IShape s; s = c;`), a
  different code path from the declaration-initializer (`IShape s = c;`) that was routed.
- `MainListener.h:14642` - the call-argument boxing path.

**Why it matters despite no live defect.** The entire `as`-boxing bug family existed because
four copies of this bookkeeping drifted apart, each carrying a different subset of the
guards. Two copies remain. The next guard added to the helper will silently not apply to
them, which is exactly how the family started. Verified 2026-07-29: the assignment-statement
leg's ownership bookkeeping lives elsewhere and happens to agree with the helper's, including
the use-of-moved diagnostic.

### Two sharp edges - one CLOSED, one NARROWED

Both were inert (no reachable failure - the ledger's one consumer, `FatValueOwnsHeapBox`, was
gated behind `FrameLocalDataOfFatValue(right) == nullptr`, which rejected every alloca-rooted
data pointer before the lookup ran), but were closed as preventive hardening so a future
second consumer of the ledger does not inherit them silently:

- `ClassifyInterfaceBoxSource` (`MainListener.h`) now tests `isa<AllocaInst>` ->
  `FrameStorage` BEFORE the ownership test, so a by-value class local whose binding is
  `IsOwning` is classified `FrameStorage`, not `Heap`.
- `FindInterfaceBoxByDataPointer` (`LLVMBackend.h`) now takes an `InterfaceBoxSource`
  parameter and filters on it; the old unfiltered single-argument overload was deleted, so
  there is only one data-pointer lookup and a caller must say which kind of box it means. Its
  one caller, `FatValueOwnsHeapBox`, now passes `InterfaceBoxSource::Heap`.

That NARROWS the second edge rather than closing it. `RegisterInterfaceBox` still dedupes on
`FatValue` only, so two records sharing BOTH a `DataPointer` and the same `Source` still
resolve to whichever was registered first. Be clear about which case that leaves: the
motivating example - two interfaces boxed over ONE object - lands on the SAME `Source`, so
the filter does not discriminate it at all. It is harmless today only because such a pair is
either same-`Source` (both `Global`, both `FrameStorage`) or impossible: a second box off an
owning binding is a hard `use of moved variable` error. Closing it properly means keying the
dedupe on `(FatValue, DataPointer, Source)`.

Verified behaviourally invisible: the fix binary against master on the ~67-program corpus
under `scratch/rev*` (interface boxing/ownership/dangle shapes), plus `Test/test_interface.cb`
and `Test/errors/err_return_interface_value.cb` - identical exit codes and stdout in every
case.

## Fix direction

In this order, because each step makes the next one cheaper:

1. **Route the two remaining open-coded sites through `BoxConcreteIntoInterface`**, then
   restore the "this is the only place" claim in its comment. This is shape 3, and it is the
   prerequisite for everything below: a value-keyed ownership rule cannot be applied from one
   place while two sites bypass that place.

2. **Make ownership follow the VALUE rather than the NAME** (shape 1). Do NOT extend
   `SoleCastOperandOf` to see through parentheses as a point fix - that closes the paren
   spelling, leaves the `?:` join, and the next shape that erases a binding reopens it. The
   provenance ledger added alongside `BoxConcreteIntoInterface`
   (`LLVMBackend::interfaceBoxRecords_`, keyed on the fat value and its data half) already
   records `Source` and `OwnershipTransferred` per box. Extend it so a box whose source was
   an owning value - however that value was spelled - retires the ORIGINAL owning temp,
   rather than requiring a named local to null. `RegisterOwnedPtrTemp` /
   `IsOwningPtrTempValue` (`LLVMBackend.h:2142`) are the existing value-keyed ownership
   machinery to build on. For the `?:` join specifically,
   `UpcastTernaryPhiToInterface` (`MainListener.h:10662`) boxes per arm and is the natural
   place to transfer per arm.

3. **Route the `??` join through the same per-arm boxing the `?:` join already uses**
   (shape 2) - or, if the join's arms cannot be recovered, reject it with the wording `?:`
   uses when an arm cannot be resolved ("bind the arm to a local variable of the class type
   first"). Either way it must not reach the module verifier.

## Related

[[return-dangle-missed-when-slot-has-extra-user]] - the live successor of the closed
`interface-return-dangle-defeated-by-intermediate-local`; its residue calls for the same
provenance-based fix as step 2 above. [[interface-issue-queue]]
