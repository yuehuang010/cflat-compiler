# A '?:' joining an owning-value STRUCT with a borrowed struct arm double-frees

Filed 2026-07-26 during step 5 (audit) of
`internal/plan/unique-interface-move-readable-null.md` - the same either-arm ownership-join
shape as the just-fixed interface fat-pointer UAF (`internal/plan/unique-interface-move-
readable-null.md` "Part 2"), confirmed to also exist for owning-VALUE STRUCTS (a by-value
struct type with a `unique` field, tracked via `IsOwningValueType` / `pendingOwnedStructTemps`,
not the pointer/interface ledgers `PropagateTernaryOwnership`'s strict rule now covers).

## Repro

```cflat
int dtorCount = 0;

class Resource
{
    int id = 0;
    ~Resource() { dtorCount = dtorCount + 1; }
};

struct UniqueBox
{
    unique Resource* item = nullptr;
};

bool identityBool(bool b) { return b; }

UniqueBox makeBox()
{
    UniqueBox b;
    b.item = new Resource();
    b.item->id = 7;
    return b;
}

void useBorrowed(UniqueBox borrowed)
{
    UniqueBox k = identityBool(false) ? makeBox() : borrowed;
    printf("k.item id=%d\n", k.item != nullptr ? k.item->id : -1);
}

extern int main()
{
    UniqueBox owner;
    owner.item = new Resource();
    owner.item->id = 42;

    useBorrowed(owner);

    printf("after useBorrowed dtorCount=%d\n", dtorCount);
    printf("owner id=%d\n", owner.item != nullptr ? owner.item->id : -1);
    return 0;
}
```

Saved at `scratch/probe_struct1.cb` (repo-root scratch, gitignored).

## Observed

```
k.item id=42
after useBorrowed dtorCount=1
owner id=42
```
then SIGABRT (exit 134) when `main` returns and `owner`'s scope-exit destructor frees
`owner.item` a second time - `k`'s destructor already freed the same `Resource` (the borrowed
arm's pointee) when `useBorrowed` returned, even though the condition selected the BORROW arm,
not the owning `makeBox()` arm. `dtorCount=1` after `useBorrowed` proves the free already
happened once at that point, one call too early.

Control (no ternary): `useBorrowedDirect(UniqueBox borrowed)` that just reads `borrowed.item`
with no `?:` leaves `dtorCount=0` and exits 0 - a plain by-value struct parameter correctly
borrows and never destructs. The bug is specific to the ternary join, not to struct-by-value
parameter passing in general (isolated with `scratch/probe_struct1b.cb`).

## Root cause

`UniqueBox` is an owning-VALUE struct (`IsOwningValueType("UniqueBox")` is true: it has a
`unique Resource*` field, so `GetOrCreateFullDestructor` synthesizes a destructor that deletes
`item`). Unlike a pointer or interface fat-pointer join, a struct-typed `?:` phi is NOT gated by
`PropagateTernaryOwnership`'s `strictJoin` predicate
(`joined->getType()->isPointerTy() || IsInterfaceFatValue(joined)`, `cflat/LLVMBackend.h:2241`) -
a `UniqueBox` phi is a plain LLVM struct type, neither a pointer nor the named
`__iface_fat_ptr` type, so it falls through to the un-strict PropagateOwnedNewTemp /
IsMovedOutPtrValue tail, none of which apply to struct-by-value ledgers at all.

Separately, the declaration-init path in `cflat/MainListener.h` that decides whether a new
struct local of an owning-value type MOVES vs COPIES vs BORROWS its initializer
(`compiler->IsOwningValueType(typeAndValue.TypeName)` gate, requiring
`srcStorage != nullptr && (isa<AllocaInst>(srcStorage) || isa<GlobalVariable>(srcStorage))` and
`!srcCallerName.empty()`) only fires for a DIRECT named-variable RHS. A ternary phi has no
`Storage`/`CallerName` (it is a pure SSA value), so that whole decision is skipped for
`UniqueBox k = cond ? makeBox() : borrowed;` and the initializer falls through to a plain
`CreateAssignment` - a shallow struct-field copy. `k` is a freshly declared local of an
owning-value type, so it unconditionally runs its full (field-deleting) destructor at scope
exit regardless of where its bits came from. When the phi selects the BORROW arm, `k` ends up
holding a bitwise copy of `borrowed.item` (the caller's live pointer) and destructs it as if it
were its own - exactly the interface bug's shape (adoption keyed on "an arm looked owning
somewhere in the join," not on which arm actually ran, compounded here by struct locals owning
unconditionally rather than by a value-identity ledger).

## Fix direction

Two independent gaps compound; either alone would still leave a smaller hole:

1. Extend the strict "every arm must be owning-or-null" join rule
   (`PropagateTernaryOwnership` / `TernaryArmJoinsOwning`, `cflat/LLVMBackend.h:2169-2265`) to
   owning-value STRUCT-typed joins, mirroring the interface fat-pointer widening: a struct arm
   only joins as owning when it is a `move`-return temp, a brace-init rvalue, or another
   provably-owning rvalue - never a plain load of a named/parameter struct local. A mixed join
   must suppress adoption (the receiving local must not run the full destructor on that value),
   matching the accepted "leak instead of double-free" trade already shipped for pointers.
2. The decl-init special-casing in `cflat/MainListener.h` (the `IsOwningValueType` /
   `srcStorage` block, ~line 8447) needs a phi/ternary-aware path: today it only recognizes a
   direct named-variable source (`isa<AllocaInst>`/`isa<GlobalVariable>` + non-empty
   `CallerName`) for the move-vs-copy decision, so a ternary RHS bypasses it entirely and always
   shallow-copies into an unconditionally-destructing new local.

## Related

- `internal/plan/unique-interface-move-readable-null.md` Part 2 (the interface fat-pointer
  fix this mirrors, commits c873f15 / c315ae0 / ba1b886 for the thin-pointer analog).
- `string` was audited in the same pass and found SOUND: `ParseTernaryBranches`
  (`cflat/MainListener.h` `FinishTernaryArm`/`AdoptTernaryStringArm`) unconditionally
  deep-copies EVERY string arm (owning or borrowed) into an independent heap buffer before the
  phi, so the joined value never aliases either arm's original buffer. Verified with
  `scratch/probe_string1.cb`: no crash, both `owner` and the ternary result print their
  original independent content.
