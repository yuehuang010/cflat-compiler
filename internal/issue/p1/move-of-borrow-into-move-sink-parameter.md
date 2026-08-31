# `move` of a borrowed pointer into a `move` SINK PARAMETER is a silent double free

## RULING 2026-08-10 (maintainer) - DEFER. btree stays as it is; unknown-accepts holds.

Raised three times during the p1 walkthrough. Studied on the third pass; the ruling is to leave
`cflat/core/hpc/btree.cb` untouched and leave this cell open.

**Why deferred rather than decided.** btree was already queued for a rewrite to minimize `if const`,
the way `list` and `dictionary` are written. That rewrite is blocked: `list` / `dictionary` are
themselves not yet in a satisfactory state, and several p1 ownership issues sit under them. Deciding
this cell now would either freeze a `move` contract around a btree that is about to be rewritten, or
force the rewrite ahead of the containers it is supposed to imitate. Revisit **after** `list` /
`dictionary` settle.

### Two corrections measured on the Release binary (`scratch/bt_gut.cb`, `scratch/bt_del.cb`)

The earlier "two unrelated jobs" framing was WRONG in both halves. `move` on a pointer parameter
does exactly ONE thing btree needs: **permission to `delete`.**

| probe | result |
|-------|--------|
| consume the pointee's owning FIELDS through a PLAIN borrow param - `dst->keys[0] = move src->keys[0]`, `_ = move n->keys[0]` | **accepted**, rc 0, source slot nulled |
| `delete` through a PLAIN borrow param | **rejected**: "cannot delete borrowed parameter 'n' - caller may own this pointer and will free it on scope exit. Declare the parameter 'move n' to take ownership." |

1. **Gut permission is not a `move` capability at all.** It is already legal through a bare
   borrow - `_releaseValueAt` (`btree.cb:199`) was proving this all along, and says so
   ("legal through the borrowed `n` (element-slot move)"). The comments at `btree.cb:969` and
   `btree.cb:1027` claiming "`node` is a `move` parameter, so `move node->keys[k]` is legal" are
   **STALE**; that is not what makes it legal. Do not act on them.
2. **btree DOES depend on `move` meaning ownership transfer**, contrary to the previous entry here.
   The "Why it is deliberate" section below is therefore accurate and is NOT superseded. Every
   `move btree_node<K,V>*` parameter in the file exists to make a `delete` legal.

### Consequence for the option set

**Option (a) - "split the two meanings, add a gut-permission modifier, drop `move` from the call
sites" - is DEAD.** There is no second meaning to split off, and dropping `move` makes four
`delete`s illegal. It was the option the governing principle pointed at; it is gone.

Inventory of `move btree_node<K,V>*` parameters and their call-site arguments:

| helper | why `move` | argument |
|--------|-----------|----------|
| `_insertChildAt` (235) | genuine transfer | fresh `new` local from `_splitChild` - **not a borrow, not in the cell** |
| `_mergeWithLeft` (963) | `delete node` (1003) | `move node`, borrowed param - in the cell |
| `_mergeWithRight` (1021) | `delete rightSib` (1059) | `move rightSib` off `parent->children[]` - in the cell |
| `_collapseEmptyRoot` (1078) | `delete node` (1083/1088) | `move node`, borrowed param - in the cell |
| `_freeSubtree` (1199) | `delete n` (1211) | plain `_root` / `n->children[i]`, **no `move` spelled** - unmeasured |

btree's usage is semantically CORRECT: the tree owns the node, the merge unlinks it, one free
happens. The compiler cannot see that owner, which is the whole difficulty.

### Surviving options, for whoever picks this up after the container work

1. **Reject `move <borrow>`, give the forwarder a spelling** - `_rebalanceFrom` declares `node` as a
   borrow it may forward ("the real owner is a structure I manage"); everything else rejected with
   no hatch. One new parameter modifier, one btree signature. This is the "Fix direction" below and
   the only survivor of the reject-family.
2. **Make the node graph `unique`**, so ownership becomes computable and the governing principle
   ("if ownership can be computed, it should be implicit" -
   the digest at the bottom of [[fix-issue-lessons]] (the 2026-08-10 uniform-implicit-move ruling)) applies with no new spelling. NOT the no-target
   dead end previously recorded here: the target would be created - `unique children[17]`, bare
   `next`, bare `path[]`. Large, and **coupled to the field-to-field ruling**, since
   `_collapseEmptyRoot`'s `_root = node->children[0]` is exactly a `unique`-field-to-field move.
   Unknown to measure first: the optimistic concurrent descent reads `children[i]` with no lock
   (`btree.cb:74-78`), and `next` is a second edge to a node `children` already points at.
3. **Unknown-accepts** - status quo, and what this ruling selects for now.
4. **Opt-in `--sanitize=ownership`.**

**Still unmeasured, prerequisites for any of 1/2:** whether `_freeSubtree`'s unspelled plain-borrow
argument into a `move` parameter is an implicit move (same hazard, no keyword); and a sweep of
`core/`, `Test/`, `example/` for other `move` pointer parameters fed from a borrow, to confirm btree
is the only dependent.

### K/V are not a factor

Neither type parameter is restricted to pointers, and neither bears on this cell. K needs
`operator<`, default-construction, and `copy()` when it is neither primitive nor pointer; V needs
default-construction plus `is_copyable(V)` for the plain `add`/`set`/`insert`, with `move` overloads
gated `if const (!is_pointer(V))`. The node graph is `btree_node<K,V>*` for every instantiation, so
the hazard and any fix are uniform across all K/V. (Unrelated small gap noticed in passing: V gets a
real `compile_error` at four sites for non-copyability, K gets none - a non-copyable owning K hits a
raw "no method `copy()`" at `btree.cb:179`.)

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

`internal/fix-issue-lessons.md` (landed design records digest) - `fix/move-borrowed-plain-dest`'s landed record, which holds the
destination table this is the last open row of.

## From the q10 bucket file (deleted 2026-08-12)

The q10 bucket is gone; this file and [[deref-of-moved-pointer-guard-inside-callee]] are what
remained of it. Two things it carried that are not already above:

- **Revisit order.** Deciding this cell now would either freeze a `move` contract around a btree
  already queued for rewrite, or force that rewrite ahead of the `list`/`dictionary` containers it
  is meant to imitate. Revisit AFTER those settle.
- **Standing constraint for the whole family.** Explicit `move x` nulls the source and leaves it
  READABLE AS NULL, by design. Do not "fix" anything here by importing Rust move semantics - that
  has been tried and breaks the suite.
