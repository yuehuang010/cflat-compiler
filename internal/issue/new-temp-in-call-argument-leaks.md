# A bare `new T()` in CALL-ARGUMENT position is never freed (32-byte leak in `Test/test_move.cb`)

Filed 2026-07-25. Pre-existing and unrelated to the recent ownership/move work - the two
allocations involved are deliberate fixtures that predate it, and the missing-free path is the
call-argument hole explicitly carved out in `internal/issue/nodiscard-residual-gaps.md`.

## Status

KNOWN / OPEN. Harm is a bounded LEAK, never a double free or a use-after-free. Deciding it
soundly needs interprocedural escape analysis (see Fix direction).

## Symptom

`Test/test_move.cb`, compiled native on macOS arm64 and run under `leaks`, reports:

```
Process NNNNN: 2 leaks for 32 total leaked bytes.
STACK OF 1 INSTANCE OF 'ROOT LEAK: <malloc in _operator new_U8Ptr_i64_>':
  ... _testUniqueLocalReassignMove_int__ ...
```

Both 16-byte blocks come from `testUniqueLocalReassignMove()`, from the two `new Resource()`
expressions that sit in ARGUMENT position:

- `Test/test_move.cb:1492` - `borrowed = borrowFirstResource(h, new Resource());`
- `Test/test_move.cb:1536` - `t4 = borrowFirstResource(t3, useNew ? new Resource() : nullptr);`

`Test/test_move.cb:1486` already calls the first one out in a comment ("the argument allocation
is deliberately unowned and leaks, so it never counts"). The second one is the same shape wrapped
in a `?:`. Nothing else in the file leaks; the rest of the suite is clean.

## Repro (minimal)

```cflat
int dtorCount = 0;
struct Resource { int id = 0; ~Resource() { dtorCount = dtorCount + 1; } };

void takeOne(Resource* a) { }

extern int main()
{
    takeOne(new Resource());          // never freed - 16 bytes leaked
    printf("dtor=%d\n", dtorCount);   // prints 0
    return 0;
}
```

`leaks -atExit -- ./repro` -> `1 leak for 16 total leaked bytes`.

Removing the construct removes the leak: `Resource* p = new Resource(); takeOne(p);` frees at
scope exit (`dtor=1`, 0 leaks), because the plain local ADOPTS the allocation.

Emitted IR for the minimal repro (`--out-lli`) - note there is no destructor and no free of `%0`
anywhere in the function:

```llvm
define i32 @main() #0 {
entry:
  %0 = call ptr @"_operator new_U8Ptr_i64_"(i64 4)
  %1 = call %Resource @_Resource_Resource__()
  %2 = extractvalue %Resource %1, 0
  store i32 %2, ptr %0, align 4
  call void @_takeOne_void_ResourcePtr_(ptr %0)
  ...
  ret i32 0
}
```

Variants confirmed to leak the same way (16 bytes each, 32 for the two-temp form):

- `takeTwo(new Resource(), new Resource());`  (32 bytes)
- `borrowFirstResource(h, new Resource());`   (the `test_move.cb:1492` shape)
- `borrowFirstResource(t3, useNew ? new Resource() : nullptr);` (the `:1536` shape)
- `if (new Resource() != nullptr) { }`        (see the second root cause below)

## Root cause

There are two independent reasons the free is not emitted, and this shape trips both.

1. A raw `new` result is ledgered for DETECTION only, never for release.
   `ParseNewExpression` records the allocation in `ownedNewTemps_` at
   `cflat/MainListener.h:14633-14634`, via `RegisterOwnedNewTemp`
   (`cflat/LLVMBackend.h:2026-2032`), whose own comment states "Detection only - never drives a
   free." That ledger exists so an assignment/adoption site can recognise an owning `new` by value
   identity (including through a `?:` arm). It is not consulted by any cleanup path.

   The machinery that actually emits end-of-full-expression frees for unowned pointer temps is
   `pendingOwnedPtrTemps` / `FlushOwnedPtrTemps` (`cflat/LLVMBackend.h:2256-2269`). Entries only
   ever get there through `RegisterOwnedPtrTemp` (`cflat/LLVMBackend.h:2017-2024`), which starts
   with `const OwnedReturnTemp* e = FindOwnedReturnEntry(value); if (e == nullptr ||
   !e->IsOwningPtr) return;`. `ownedReturnTemps_` is populated only for owning-RETURN CALL results
   (`cflat/LLVMBackend.h:1965-1988`); a raw `new` value is not in it. So a bare `new T()` temp can
   never be registered for release at ANY consuming site - not even at the two sites the Gap-2 fix
   (commit bc3ac75) does cover, which is why `if (new Resource() != nullptr) { }` also leaks while
   `if (makePtr() != nullptr) { }` does not.

2. Call-argument position is deliberately not a registration site at all.
   The registration callers are the comparison-operand sites (`cflat/MainListener.h:11373-11377`,
   `11696-11700`) and the scalar-field deref base (`cflat/MainListener.h:16226`). The contract
   comment at `cflat/LLVMBackend.h:2011-2013` spells out why arguments are excluded: "A CALL
   ARGUMENT is deliberately NOT such a site - a borrow parameter may legally RETAIN its argument,
   so freeing it here would be a use-after-free; it stays a leak." The same carve-out is recorded
   in `internal/issue/nodiscard-residual-gaps.md:19-23`, and `Test/test_collection_leaks.cb` pins
   the retaining-callee shape (`retainPtrTemp`) as a POSITIVE test so a caller-side free cannot be
   added silently.

   Consequently `takeOne(mk())` (a `move R*`-returning call in argument position) leaks 16 bytes
   for exactly the same reason - the raw-`new` form is just the un-ledgered sibling of the already
   documented gap.

The mandatory-nodiscard check does not catch either form: it inspects the full expression's RESULT
value (`DiagnoseDiscardedOwningReturn`, `MainListener.h`), and here the result is the outer call's
return (a borrow, or `void`), not the buried allocation.

## Fix direction

Two separable pieces, in increasing cost:

1. Cheap and safe: give a raw `new` the same ledger identity an owning RETURN has, so the
   already-shipped non-escaping sites cover it too. Either add `new` results to
   `ownedReturnTemps_` with `IsOwningPtr` set, or teach `RegisterOwnedPtrTemp` to accept a live
   `ownedNewTemps_` entry (carrying `TypeName` / `AllocAlignment`, both already computed at
   `MainListener.h:14626`). That fixes `if (new R() != nullptr) { }` and `new R()->scalarField`.
   It does NOT fix `test_move.cb`, since both leaks there are in argument position.

2. The argument-position case needs an escape decision per parameter. Options, cheapest first:
   - Diagnose instead of free: reject a bare `new T()` (or any un-adopted owning temp) passed to a
     non-`move`, non-`alias` pointer parameter, and require the caller to name it or spell `move`.
     Zero runtime risk; would require touching the two `test_move.cb` fixtures and any example
     that relies on the shape.
   - Infer a per-parameter "does not retain" bit during the ForwardRefScanner/codegen pass (the
     parameter is never stored to a global/field, never returned, never passed onward to a
     retaining slot) and register the argument temp in `pendingOwnedPtrTemps` only when that bit
     holds. This is the sound version and is the interprocedural analysis the existing comment
     defers.

Do NOT simply start freeing every owning temp in argument position - that turns the documented
`retainPtrTemp` positive test into a use-after-free. A leak is the accepted trade here; a
use-after-free is not.

## Relationship to recent work

Unrelated to the ownership/move changes on `master` (bc3ac75 and earlier). bc3ac75 narrowed this
family by covering comparison operands and scalar-field derefs, and it explicitly left call
arguments alone. Both `test_move.cb` allocations were already leaking before that commit, and the
first is annotated as expected in the test source itself.
