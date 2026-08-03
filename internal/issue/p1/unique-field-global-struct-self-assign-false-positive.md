# Two GLOBAL struct receivers' `unique` field copy is misread as a self-assign, no diagnostic

Filed 2026-08-02 by the round-1 review of `fix/uniq-array-elem`, found while correcting a false
claim about global teardown in that branch's test comment. **Pre-existing** - measured identical
on `58d5d27` and on the branch, so it is neither caused nor worsened by that work.

Severity: silent abort (exit 134), no diagnostic at all. The identical copy between two LOCAL
receivers is correctly rejected on both binaries.

> **Mechanism corrected 2026-08-02 (round-2 review); the file was renamed with it.** The first
> version claimed the guard stack was never ENTERED, because `destIsStructField`'s
> `GetElementPtrInst` cast failed on a folded global address. That was read off OPTIMIZED IR and
> is FALSE - at emission (`--no-opt`) the address is a real `GetElementPtrInst`. The account
> below is re-derived from `--no-opt` IR plus black-box controls and says the opposite: the stack
> IS entered, and a self-assign discriminator suppresses it. **Read `--no-opt` IR when reasoning
> about what the compiler emits** - optimized output folds away the very GEPs the reasoning is
> about. The old file also claimed `gArr[0].slot` fails the same cast; that too was wrong.

## Repro

```cflat
int gfreed = 0;
struct Node { int v = default; ~Node() { gfreed = gfreed + 1; } };
struct Holder { unique Node* slot = nullptr; };
Holder gA = default;
Holder gB = default;
extern int main()
{
    gA.slot = new Node(); gA.slot->v = 7;
    gB.slot = new Node(); gB.slot->v = 8;
    gA.slot = gB.slot;
    printf("v=%d freed=%d\n", gA.slot->v, gfreed);
    return 0;
}
```
```
v=8 freed=1
```
compile rc 0, run rc 134 on BOTH binaries. The same copy between two LOCAL `Holder`s is rejected
on both - that control is what makes this a receiver-kind hole rather than a gate-width one.

## Root cause - established by three black-box controls plus `--no-opt` IR

The guard stack IS entered. `--no-opt` IR for `gA.slot` is
`getelementptr inbounds %Holder, ptr @gA, i32 0, i32 0` - a `GetElementPtrInst`, 2 indices,
struct source element type - so `destIsStructField` is TRUE, and the `uq.delete` block that
`EmitUniqueFieldDelete` emits (itself gated on `destIsStructField`) is present in the output.

What suppresses the reject is `selfFieldAssign` in `ParseAssignmentExpression`
(`cflat/MainListener.h`), which concludes self-assign from equal `FieldName` plus equal
`CallerName`. A field read off a GLOBAL struct carries an EMPTY `CallerName`, so two different
globals compare equal - the identical failure mode as
[[interface-field-self-assign-false-positive]], reached through a third receiver kind.

Three controls localize it, each measured on both binaries:

| shape | result |
|---|---|
| global SOURCE into a LOCAL destination (`b.slot = gA.slot`) | **rejects** - and prints the access as `'slot'` with no caller, which is the empty `CallerName` showing through |
| LOCAL source into a GLOBAL destination (`gA.slot = b.slot`) | **rejects** (prints `'b.slot'`) |
| global into global (`gB.slot = gA.slot`) | **no diagnostic** |

Neither half is individually blind; only the pair is. The decisive control is renaming the two
fields apart - `struct HolderAB { unique Node* a; unique Node* b; }` with `gA.a = gB.b` - which
**rejects on both binaries**. Equal field name plus two empty caller names is the whole of it.

## Why the `CallerName` is empty - a lookup asymmetry, not an oversight at one site

Established 2026-08-02 by reading the declarations. Locals and globals resolve through two
different structures with different richness:

```
stackNamedVariable  : std::vector<StackState>                                (LLVMBackend.h:1526)
                      StackState::namedVariable / functionArgument
                          : name -> NamedVariable                            (LLVMBackend.h:988)
globalNamedVariable : std::unordered_map<std::string, llvm::GlobalVariable*> (LLVMBackend.h:1537)
```

A local resolves to a full `NamedVariable`, which carries `Storage` ("the container holding the
value, used to load or store", `LLVMBackend.h:806`), `CallerName` (`:861`) and `FieldName`
(`:863`). A global resolves to a bare `llvm::GlobalVariable*` - there is no `NamedVariable` in
that map at all, so there is no `CallerName` to carry. The empty string is not a missed
assignment at one materialization site; it is what a receiver kind with no `NamedVariable`
representation necessarily produces.

That reframes the fix. The scope stack already IS the identity mechanism this guard wants -
`Storage` is the slot, and `ProvablyDifferentSlots` (added by `fix/uniq-array-elem`) already
consumes `rightNV.Storage`. The name comparison is a legacy proxy layered on top of a primitive
that already exists for locals and is simply absent for globals.

## Fix direction - not attempted here, and why

`fix/uniq-array-elem` added exactly the kind of discriminator this needs,
`ProvablyDifferentSlots(destination, rightNV.Storage)`: it flattens both addresses to a root and
proves difference from differing constant offsets. It does not fire here only because it
requires the two roots to be the SAME value; here they are two DISTINCT `GlobalVariable`s.

Two distinct `GlobalVariable`s (and two distinct `AllocaInst`s) are distinct objects in LLVM, so
"different roots, both of those kinds" is a sound proof of difference and would close this in
about two lines. It was deliberately NOT done on that branch: it widens the proof rule that
every field store flows through, and landing a widening on the last available review round with
no differential sweep and no must-still-work corpus for distinct-root pairs is the kind of
unswept tightening `internal/fix-issue-lessons.md` records going wrong repeatedly. Do it as its
own change, with the sweep.

The proof must stay keyed on the ROOT KIND, not on inequality of two `Value*`s: two `LoadInst`s
can name one object, which is exactly why `SameLoadedPointer` exists beside it.

**The preferred order, given the asymmetry above.** Two steps, and the first one alone may be
enough:

1. **Give a global receiver the same `NamedVariable` representation a local already has**, so
   every receiver kind carries a real `Storage` and a non-empty identity. This is ADDITIVE: a
   receiver that today has no identity gaining a correct one cannot false-reject any program
   that works now, which is the opposite risk profile from widening the name predicate (that
   widening was attempted on a sibling issue and false-rejected working programs - see
   [[interface-field-self-assign-false-positive]]). Once globals carry `Storage`, the existing
   root-kind proof covers this repro with no change to the predicate at all.
2. Only if step 1 leaves gaps, extend the root-kind proof to distinct `GlobalVariable` /
   `AllocaInst` roots as described above - with the differential sweep and the
   distinct-root must-still-work corpus.

Unverified and worth checking first, since it sets the size of step 1: whether any path already
wraps a global into a `NamedVariable` (which would make this mostly plumbing), and whether
`Storage` is populated consistently for every receiver kind the field-store guards see - an
array element behind a pointer, an array view, and a temp spill each reach `Storage`
differently, and `fix/uniq-array-elem` needed `SameLoadedPointer` to treat two loads of one
address as one root.

**This route does NOT close the interface face**, and that is recorded rather than assumed:
[[interface-field-self-assign-false-positive]] measured that comparing the interface locals'
`Storage` false-rejects `ISlot ia = a; ISlot ib = a;` (two boxes of ONE object, two distinct
allocas). That face needs the boxed DATA pointer resolved to its root store, which is a related
but separate proof. Three receiver kinds share the empty/ambiguous-`CallerName` root cause;
they do not share one remedy.

Wants an `expect_error` leg once fixed, plus a must-keep-working leg for `gA.slot = gA.slot`
(a genuine self-assign on one global - compiles, prints `freed=0`, rc 0 on both binaries today).

Related: [[unique-field-to-field-array-element-receiver]],
[[interface-field-self-assign-false-positive]], [[interface-issue-queue]]
