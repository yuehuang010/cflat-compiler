# `_ = move <element>` cannot release a container slot that `T tmp = move <element>` can

Filed 2026-07-23. A disparity between the two explicit move-out forms when the SOURCE is a
container element slot (single-index GEP lvalue):

- `T tmp = move _data[i];` - RELEASES an owning element correctly (thin `unique T*` deleted,
  `unique <iface>` vtable-freed, value/string destructed), and leaves a dead slot.
- `_ = move _data[i];` - SILENTLY STRIPS ownership: an owning element is neither freed nor
  transferred, so its pointee LEAKS.

They are meant to be the same operation ("move the element to nowhere = release it"), so the
divergence is a soundness gap in the discard form.

## Repro

```cflat
// queue<unique Box*> holds owning elements. Releasing slot i:
T tmp = move _data[i];   // OK: Box deleted exactly once (Part 6 STEP D collapse uses this)
_ = move _data[i];       // LEAK: the Box is never deleted
```

Empirically: the `T tmp = move` form is what queue/stack/list `_releaseAt` and dictionary
`_releaseValueAt` were collapsed to (leak matrix 352/352 HeapAudit-clean). Substituting
`_ = move _data[i]` there regresses to leaks for every owning element kind
(`unique T*`, `unique <iface>`, value/string).

## Root cause

Container element ownership (`unique` vs borrow) is a GENERIC-TYPE-BINDING property. It lives
only in `activeTypeSubstitutions` - what `is_unique(T)` / `IsUniqueTypeArg` read - and is ABSENT
from the element lvalue itself: `queue<unique Box*>` and `queue<Box*>` share a byte-identical
`Box**` buffer, and a slot read demotes to a bare `Box*` (`IsUnique = 0`).

- `T tmp = move _data[i];` has a DESTINATION TYPE (`T`), so ownership is recoverable via
  substitution. Part 6 STEP D added exactly this: a `lastMovedFromContainerSlot` flag (set on an
  `IsElementAccess` move, reset at `ParseAssignmentExpression` entry and in `ResetForReanalysis`)
  gates a decl-init override that RE-DERIVES the dropped local's ownership from the destination
  type `T` (`IsUniqueTypeArg`). See `ParseMoveExpression` / `ParseDeclaration` and the plan
  `internal/plan/ownership-transparent-assignment.md` (2026-07-23 STEP D entry).
- `_ = move _data[i];` has NO destination type. The `_ =` discard branch of
  `ParseAssignmentExpression` intercepts only a top-level `move` over a BARE IDENTIFIER naming a
  live local (releases it via `DropValue`, nulls the slot, marks it moved) - it does NOT handle a
  subscript lvalue source, and it has no `T` to re-derive element ownership from. So the demoted
  bare `Box*` is dropped as a non-owning pointer -> leak.

## Fix direction

Teach the `_ =` discard path to release a container-element source with the same
element-ownership recovery the decl-init path uses:

1. In the `_ = move <expr>` interception, recognize an `IsElementAccess` / single-index-GEP source
   (the same signal `lastMovedFromContainerSlot` already sets), not just a bare identifier.
2. Re-derive the element's ownership from the container's `activeTypeSubstitutions` binding
   (`IsUniqueTypeArg` for the element `T`), exactly as the decl-init override does, and release via
   `DropValue`-equivalent for that kind (thin unique delete / unique-iface vtable free / value or
   string full dtor).
3. Zero the slot after release so a later teardown pass over the (now dead) slot is a no-op -
   matches how the `T tmp = move` collapse leaves the slot.

Payoff: unifies the two spellings and unblocks collapsing the remaining teardown-side ladders
(`_releaseAt` / `_freeValue`) to the discard form (Part 6 STEP D was forced onto `T tmp = move`
solely because of this gap). Guard the change with the `Test/test_collection_leaks.cb` leak
matrix (every element kind) - a mis-recovery flips a leak into a double-free.

## Related
- `internal/plan/ownership-transparent-assignment.md` - Part 1 (`_ = move` discard release form)
  and Part 6 STEP D (the `T tmp = move` element collapse that this gap forced).
