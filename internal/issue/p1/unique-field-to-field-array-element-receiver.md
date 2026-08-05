# `unique` field-to-field copy between ARRAY ELEMENTS - NARROWED to runtime indices

Filed 2026-08-01 by the round-1 review of `fix/uniq-family`. **NARROWED 2026-08-02** on
`fix/uniq-array-elem`: every shape whose two element addresses are provably different FROM THE
EMITTED ADDRESSES is now rejected. What remains is every shape where the index is not a
compile-time constant in the IR - a runtime subscript, and also a `const`-declared integer.

Severity of the residue: silent abort (exit 134), no diagnostic - unchanged for those shapes.

## Root cause - CONFIRMED 2026-08-02 by instrumentation, not by description

The filed hypothesis held exactly as written. Measured by printing the two `NamedVariable`s at
the top of the field-store stack in `ParseAssignmentExpression`, for
`arr[0].slot = arr[1].slot`:

```
destField='slot' destCaller='arr' | srcField='slot' srcCaller='arr'
selfFieldAssign=1 selfUniq=1 isUniqRead=1
```

`FieldName` and `CallerName` name the CONTAINER on both sides, so `selfFieldAssign` concluded
self-assign and `selfUniqueFieldAssign` suppressed the whole guard stack. `IsUniqueFieldRead`
was already TRUE for the source, so the field-to-field reject was reached and suppressed - the
gate was never the problem, only the discriminator. Two controls confirm it independently: the
same copy with the two fields NAMED APART (`arr[0].a = arr[1].b`) and with two DIFFERENT arrays
(`arrA[0].slot = arrB[0].slot`) both rejected on the pre-fix binary.

## What is CLOSED

`selfFieldAssign` now also requires `!ProvablyDifferentSlots(destination, rightNV.Storage)`
(`cflat/MainListener.h`). The proof flattens both addresses through all-constant-index GEPs to a
root and rejects only when the roots match and the byte offsets DIFFER; two roots that are
separate `LoadInst`s of the same address in one basic block with no intervening memory write are
treated as one root, which is what reaches an array behind a pointer or an array view.

Closed shapes, each measured compile rc 1 with the ELEMENT spelling in the message, and each
with an `expect_error` leg in `Test/errors/err_unique_array_element_field_to_field.cb`:
local array, generic-substituted `Box<unique Node*>[2]`, nested `arr[0].h[1].slot`,
array-as-struct-field `o.arr[1].slot`, through-pointer `p->arr[1].slot`, array view
`v[1].slot`, global array. Namespace-qualified and `using`-aliased struct spellings ride the
same arm (measured, no separate leg). A pointee WITHOUT a user destructor behaves identically.

The same blind spot reached the OTHER copy of the same name comparison and is closed with it.
There are exactly two: `selfFieldAssign` (the diagnostics) and `sameFieldStore` (the
reassignment-destruct that frees the OLD destination, and the auto copy). Fixing only the first
traded the abort for a LEAK: an owning field copied element-to-element stopped aborting but
never released the old buffer (`leaks --atExit`: 1 leak / 16 bytes, where the named-local
spelling reports 0). `Test/test_move.cb` measures 13 leaks / 256 bytes on the pre-fix binary and
13 / 256 on the fixed one; the intermediate state measured 14 / 272.

**The two flags are deliberately NOT the same predicate.** A diagnostic reads no memory, so
`selfFieldAssign` uses the bare proof. The copy and destruct paths EMIT code that reads and frees
the slot, so `sameFieldStore` additionally requires both addresses to root in stack or global
storage (`AddressRootIsStackOrGlobal`). Without that, a receiver reached through a raw-malloc'd
pointer had its garbage contents deep-copied and destructed: measured, a program that ran clean
on the pre-fix binary SIGSEGV'd (exit 139). This is the same trade, and the same polarity, the
closure store already documents a few lines above - a leak is recoverable, corrupting the heap is
not. The consequence is a real LIMIT: a POINTER or array-VIEW receiver keeps the pre-fix aliasing
for owning-value fields (7 of 9 measured owning-value shapes are fixed; `ptr` and `view` are
unchanged from pre-fix, still exit 134). The `unique` REJECT is a diagnostic and DOES still fire
for those two receivers.

The third name comparison in the same block, `selfUniqueFieldAssign`'s second arm, compares
`TypeAndValue.VariableName` but requires BOTH `FieldName`s to be EMPTY. An element access always
carries a non-empty `FieldName`, so that arm is structurally unreachable for this receiver shape
and needs no change. No other site in `cflat/` compares receiver identity by name equality
(`CallerName`, `ParentVariableName` and `VariableName` were each grepped; the remaining
`ParentVariableName` uses are reads for diagnostics and borrow tracking, never an equality test
between two sides of a store).

## What the regression legs do and do NOT guard

The destruct half is guarded by `uae_elem_reassign_destructs_old` in
`Test/test_move.cb::testUniqueArrayElementFieldStore`, built on `ElemBuf` - a copyable owning
value with a COUNTING destructor, because a plain `string` field's destructor is not observable
in-language and a string leg would pass on every binary. Deleting `&& !provablyDifferent...` from
`sameFieldStore` fails exactly that one leg.

The REJECT half is guarded ONLY by `Test/errors/err_unique_array_element_field_to_field.cb`. Under
a full revert to the pre-fix compiler ZERO value legs fail, because the binary aborts before
producing leg output at all. Do not read the value legs as protecting the rejection.

## What REMAINS - the runtime-index shape within ONE array, deliberately accepted

```cflat
int gfreed = 0;
struct Node { int v = default; ~Node() { gfreed = gfreed + 1; } };
struct Holder { unique Node* slot = nullptr; };
extern int main()
{
    Holder[2] arr = default;
    int i = 0; int j = 1;
    arr[i].slot = new Node(); arr[i].slot->v = 7;
    arr[j].slot = new Node(); arr[j].slot->v = 8;
    arr[i].slot = arr[j].slot;
    printf("v=%d freed=%d\n", arr[i].slot->v, gfreed);
    return 0;
}
```
```
v=8 freed=1
```
compile rc 0, run rc 134 - identical before and after the fix.

This is NOT an oversight and must not be closed by widening the rule. `i` and `j` can hold the
same value, and `arr[i].slot = arr[i].slot` is a legal self-assign that compiles and frees once
today. Rejecting an unprovable pair would take away working programs, which is the failure
mode the `interface-field-self-assign-false-positive` landed record (in
[[interface-issue-queue]]) records an attempt at. The must-keep-working
shapes are value-asserted in `Test/test_move.cb::testUniqueArrayElementFieldStore`
(`uae_rt_self_*`, `uae_rt_diff_*`, `uae_const_self_*`, `uae_ptr_rt_self_*`); an over-broad
polarity was mutation-tested against them and all four flipped to compile errors.

**Narrowed again 2026-08-04 by `fix/uniq-global`.** The residue is no longer "any index not
constant in the emitted IR": it now excludes the pair of DISTINCT GLOBAL arrays.
`gArrA[i].slot = gArrB[j].slot` on two different file-scope arrays flipped accept -> reject
there (measured: accepted and aliased on `a846e6e`), since the distinct-roots arm proves two
objects apart without looking at the indices at all. Two distinct LOCAL arrays already rejected
before that change - their `CallerName`s differ - so nothing moved for them. What is left is
strictly the ONE-array case above, where a single root leaves nothing to prove, and that is
exactly the case where `i` and `j` may be equal.

Closing that needs a runtime owner check, not a stronger compile-time proof - i.e. the
store would have to compare the two slot addresses and skip when equal, which is a different
piece of work (a codegen change with a cost, not a diagnostic).

## Also residue - a `const` integer index, and why the proof was NOT extended

```cflat
const int K = 1;
arr[0].slot = arr[K].slot;
```
compile rc 0, run rc 134 on BOTH binaries - undiagnosed. `arr[1+1]` IS folded by the front end
and IS rejected, so the boundary is "constant in the emitted GEP", not "constant in the source".

The proof was deliberately NOT extended to fold a `const`-declared local. `const` is not
enforced in CFlat today: `const int K = 1; K = 5;` compiles clean (measured). Folding K's
initializer into a provable index would therefore be UNSOUND - the value can be reassigned
between the declaration and the access, so two accesses spelled `arr[K]` need not name one slot,
and the rejection would rest on a false premise. Closing this needs `const` to actually be
enforced first; that is a language change, not a widening of this guard.

## Two notes for whoever touches these predicates next (round-3 review, 2026-08-02)

- The by-value-string-param deep copy (the first of the four `!sameFieldStore` sites) has a DEAD
  term: that guard already requires `rightNV.FieldName.empty()`, while `sameFieldStore` requires
  a non-empty `FieldName` equal on both sides. The two contradict, so the term was always false
  before the change and is always false now. Not a regression, but a future reader will assume it
  does something.
- `SameLoadedPointer` (two loads of one address counting as one root) is structurally
  DIAGNOSTIC-ONLY: if the root is a load, `AddressRootIsStackOrGlobal` is false by construction,
  so the relaxation can never reach the `-Initialized` form the emitting sites use. That is what
  the design intends; stating it in the code would save the next reviewer the derivation.

## Also out of scope

- The GLOBAL NAMED struct receiver (`gA.slot = gB.slot` on two file-scope `Holder`s) was out of
  scope here because its addresses have two DISTINCT roots, which this fix's proof deliberately
  did not treat as different. **FIXED 2026-08-04** by `fix/uniq-global`, which added
  `ProvablyDifferentObjects` beside the same-root offset rule: two distinct `AllocaInst`s /
  `GlobalVariable`s are distinct objects whatever the indices in between. The runtime-index
  residue below is untouched by it - a single array has ONE root, so the new arm cannot fire.
  See the landed design record in [[interface-issue-queue]], including why that change did NOT
  give globals a `CallerName` (the empty string is load-bearing at two funcptr checks).
- The INTERFACE receiver of the same mechanism was its own file
  (`interface-field-self-assign-false-positive`) and was **FIXED 2026-08-05** by
  `fix/iface-selfassign` - see the landed design record in [[interface-issue-queue]]. An interface
  field has no address with a constant offset to compare, so this fix's proof could not reach it;
  that one resolves each receiver's fat pointer back to the OBJECT its box wraps and then reuses
  `ProvablyDifferentObjects`. Nothing here moved in either direction.
- The `move` remedy names an expression ONLY when one is known to be right. Two review rounds
  were spent on this arm, so the rule is now stated as an invariant: `ExactUniqueFieldAccess`
  returns the written RHS text for a plain indexed lvalue path, the name-derived
  "<caller>.<field>" only when it IS what the user wrote, and otherwise EMPTY - in which case the
  message names no expression at all and says "prefix the source expression with 'move'".
  Keying it on the GEP shape was wrong (a zero index folds its element GEP away, so four of six
  element-0 sources printed `move o.slot` / `move p.slot` / `move slot` / `move a.slot`, none of
  which compile). Keying it on text alone was also wrong: `arr[(int)1].slot` fell back and printed
  `move arr.slot`, which DOES compile and silently transfers the wrong element. Casts and
  arithmetic inside an index are now admitted; a parenthesized whole expression
  (`(arr[1]).slot`) is not, and takes the no-expression wording. Every arm was run end to end -
  compile rc 0, correct value moved in, source nulled, freed exactly once, 0 leaks.

Related: [[unique-field-to-field-interface-receiver-residues]], [[interface-issue-queue]]
