# Mixed owning/borrow struct '?:' join leaks the owning arm

## Summary

`Own b = h ? borrowedLocal : owningTemp;` (either arm order) compiles, borrows, and LEAKS
everything the owning arm allocated whenever the owning arm is the one taken. This is the
already-shipped trade for mixed pointer/interface joins, extended to owning-value structs; it
is deliberate and load-bearing, not an oversight. The record exists so it is not re-diagnosed.

## Repro

```cflat
import "list.cb";
struct Node { int v = 3; };
struct Own { string name = "abc"; unique Node* n = new Node(); list<int> xs = default; };
Own make() { Own r = default; r.n->v = 7; return r; }
extern int main(int argc, char** argv)
{
    bool h = argc > 1;
    Own other = default; other.n->v = 9;
    Own c = h ? other : make();     // or `: default` - identical machinery
    printf("%d\n", c.n->v);
    return 0;
}
```

`leaks --atExit` on macOS Release: no argument (owning arm taken) reports 1 ROOT LEAK of 16
bytes in `_operator new_U8Ptr_i64_` (the `Node`); with an argument (borrow arm taken) it is
clean. The `default` spelling and the `make()` spelling produce byte-identical behaviour at
every destination, so `default` adds no new hole.

## Root cause

`PropagateTernaryOwnership` (`LLVMBackend_OwnershipTemps.cpp`) scores the join mixed when one
arm fails `TernaryArmJoinsOwning`, calls `SuppressCallerRelease` and
`RegisterNonOwningStructJoin`. The local-declaration receiver
(`MainListener_Declarations.cpp`, the `MIXED '?:' join of an owning-value STRUCT` block) then
marks the new local `IsAliasBorrow` with `IsOwning = false`, so no scope-exit destructor is
emitted for it. The owning arm's temp survives the arm (`FinishTernaryArm` keeps the yielded
value) and nothing ever frees it.

The leak is the price of safety, not a missing free: the destination holds the joined BITS and
outlives the statement, so freeing the owning arm's temp at the end of the full expression
would dangle the destination on the very path that allocated. Destructing unconditionally
would double-free the borrow arm's live pointee, which its real owner frees again.

Other destinations already reject the same join outright with
`RejectNonOwningStructJoinStore` (field store, owning-variable assignment); only the fresh
local declaration borrows silently.

## Fix direction

Needs conditional ownership, or a copy: either
- a runtime live-flag join (the `PendingOwnedStructTemp::LiveFlag` mechanism already used by
  `HoistOwnedStructTempTo` generalized to the join, with the destructor registered at the
  DESTINATION's scope rather than at the end of the full expression, which requires the join to
  know its destination); or
- an owning-struct deep-copy facility so the borrow arm can be promoted to owning and the join
  made uniformly owning, the way `AdoptTernaryStringArm` already does for `string` arms.

Do NOT "fix" it by freeing the owning arm's temp at statement end - that converts the leak into
a use-after-free on the taken path, which is strictly worse.

## Related, separate

`take(makeOwn())` - an owning-value struct temp in plain ARGUMENT position, no ternary and no
`default` involved - is also never destructed. Same measurement run, different gap.
