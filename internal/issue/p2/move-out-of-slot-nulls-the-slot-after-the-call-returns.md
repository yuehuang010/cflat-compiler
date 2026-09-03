# `f(move obj->slot)` nulls the slot AFTER `f` returns, so a callee that reseats the slot is overwritten

Filed 2026-09-03 while landing fdb7ffa (slot-move rule). Found by bisecting a real failure:
rewriting `_collapseEmptyRoot(node->isLeaf, move node)` in cflat/core/hpc/btree.cb to the slot
form `_collapseEmptyRoot(node->isLeaf, move _root)` made every `remove()` after the first root
collapse return false (500-key ascending removal stalled at key 372 with 129 keys left).
fdb7ffa worked around it by making the helper RETURN the replacement and assigning `_root` at
the call site; the ordering itself is unfiled and undocumented.

## Repro

```cflat
struct Node { int v = 0; };
struct Tree
{
    Node* root = default;
    void collapse(move Node* old) { delete old; root = new Node(); }   // reseats the slot
    void shrink() { collapse(move root); }
};
extern int main()
{
    Tree t = default; t.root = new Node();
    t.shrink();
    return t.root == nullptr ? 1 : 0;   // observed 1: the reseat was overwritten with null
}
```

Expected by analogy with C++ (`std::move` leaves the source in a valid-but-unspecified state
BEFORE the callee runs; `Option::take()` in Rust empties the slot before the call) is that the
slot reads null INSIDE the callee and any store the callee makes to it survives.

## Root cause (CONFIRMED 2026-09-03: repro exits 1; `--symbol-dump-ir function:shrink` shows `store ptr null` on the line AFTER the `call`)

The "explicit move nulls the source" store for a `move <lvalue>` argument is emitted in the
caller's post-call sequence (the same place the moved-from local is marked consumed), not
before the call instruction. For a LOCAL that is unobservable - the callee cannot reach the
caller's alloca. For a FIELD or ELEMENT slot reached through a pointer the callee can alias the
slot (`this->root` here), so the late store clobbers whatever the callee wrote. Look at the
argument-side move handling in CreateOverloadedFunctionCall (LLVMBackend_Overloads.cpp) and the
consume-source emission in MainListener_Expressions.cpp; `--symbol-dump-ir function:shrink`
should show the `store null` after the `call`.

## Fix direction

Emit the null store BEFORE the call for slot (non-local) move sources: load the value, store
null into the slot, then pass the loaded value. Locals may keep the current order (no
observable difference) or move too for uniformity. Then: positive leg in Test/test_move.cb
(callee reseats the slot it was moved from; assert the reseat survives and dtor count is 1),
revert btree's `_collapseEmptyRoot` to reseat `_root` itself if that reads better, and add one
sentence to doc/LANGUAGE.md's "Moving into a move parameter: the slot rule" section stating
when the slot becomes null. Standing constraint: explicit `move x` nulls the source and leaves it
readable-as-null by design - this issue changes WHEN, not WHETHER.

Related: fdb7ffa digest entry in internal/fix-issue-lessons.md, [[explicit-move-nulls-source]],
[[move-borrow-into-sink-slot-rule]].
