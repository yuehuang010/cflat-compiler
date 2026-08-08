# `move` of a borrowed pointer into a `move` SINK PARAMETER is a silent double free

P2, PRE-EXISTING and currently DELIBERATE. Measured identical on `f1b8116` and on
the `fix/move-borrowed-plain-dest` branch head: rc 133, no diagnostic. Filed 2026-08-08 by
`fix/move-borrowed-plain-dest` so the deliberate hole is tracked rather than only narrated.

## What

`fix/move-borrowed-plain-dest` made a plain `T*` destination decline to adopt a moved borrow and
made a `move` RETURN type reject one. The remaining destination - a callee's `move` PARAMETER -
still accepts it, so the callee frees a pointee the caller still owns.

## Repro (rc 133 on both binaries)

```cflat
int dtorCount = 0;
class Ci { int r = 7; ~Ci() { dtorCount = dtorCount + 1; } };
void sink(move Ci* q) { delete q; }
void f(Ci* p) { sink(move p); }
extern int main() { Ci* c = new Ci(); f(c); delete c; return 0; }
```

`l.add(move p)` into a `list<unique Ci*>` is the container spelling of the same cell.

## Why it is deliberate, and what makes it hard

This is the one place the ratified "the programmer asserts the borrow is dead" policy still has
force, and `cflat/core/hpc/btree.cb` depends on it: `_rebalanceFrom` (lines 929 / 952) hands a
borrowed `btree_node<K,V>*` parameter to `_collapseEmptyRoot` / `_mergeWithLeft`, both of which
declare it `move`. That code is CORRECT - the tree, not the caller's local, owns the node - and it
has NO remedy available: `_rebalanceFrom` cannot declare `node` a `move` parameter, because it only
conditionally hands the node off and rebinds it (`node = parent;`) each turn of the loop. A blanket
rejection here therefore breaks `core/` for every program that imports btree.

## Fix direction

Not a guard. Either an opt-in check (a `--sanitize=ownership` site, which already records move
origins), or a way for a function to declare that a pointer parameter is a borrow it may forward -
so `_rebalanceFrom` can state what it is doing and everything else can be rejected. Do NOT retry a
destination-agnostic rejection at the `move` expression; that was measured and disproved.

## Related

[[interface-issue-queue]] - `fix/move-borrowed-plain-dest`'s landed record, which holds the
destination table this is the last open row of.
