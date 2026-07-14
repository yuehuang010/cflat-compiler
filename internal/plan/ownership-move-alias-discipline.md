# Ownership: the `move` / `alias` discipline, and the two ways to get it wrong

Created 2026-07-14 (from building `core/hpc/btree.cb`). **This is a design note, not a work
item. Nothing here is planned or scheduled.**

Ownership is a property of a single owning **name** (`NamedVariable.IsOwning`). The compiler
does not perform alias or escape analysis, and is not going to. **The library writer is
responsible for knowing whether their struct or class owns or borrows its pointer fields, and
for spelling that intent with `move` and `alias`.** This file records the discipline and the two
traps that catch people who do not follow it.

Related: [aligned-new-field-escape](../issue/aligned-new-field-escape.md) - the same
name-centric model losing the *alignment* tag on field assignment.

## What the model actually does (verified, not assumed)

Ownership **does** transfer on assignment, wherever there is ownership to transfer:

| assignment | transfers? |
|---|---|
| owning local -> struct field (`h->slot = fresh;`) | **yes** - `fresh` is not freed at scope exit |
| owning local -> array element (`h->slots[0] = fresh;`) | **yes** |
| `move` parameter -> field (inside the callee) | **yes** |
| **borrowed** parameter -> field | **no - there is nothing to transfer.** The caller still owns it, and still frees it |
| field -> plain local copy (`Node* p = h.slot;`) | **no** - the copy is a borrow; the field keeps ownership |

So the model is coherent. Neither failure mode below is the compiler "losing track" of ownership
- both are the compiler **declining to check** something. That is the distinction that makes the
discipline the right answer rather than a workaround.

## The discipline

**`move` on a parameter means "I take ownership."** A function that deletes a pointer parameter,
or stores it into a structure that outlives the call, MUST declare that parameter `move`.

**A parameter is `move` only when the function frees or stores it on EVERY path.** If the free is
conditional, keep the parameter borrowed and forward `move` from the branch that commits.

**`alias` on a return means "you do not own this - do not free it."** `dictionary.cb`'s
`alias V get(K key)` is the reference example.

**`delete` and `move` target a genuine owning local.** Not a pointer copied out of a structure.

**Forwarding a borrow onward as `move` is legal, and is the sanctioned way to transfer ownership
out of a borrow.** The checker is deliberately asymmetric:

- `delete borrowedParam` is an ERROR ("cannot delete borrowed parameter",
  `MainListener.h:11313-11324`), and `IsBorrowed` propagates through a plain local copy, so
  laundering it through a local does not help.
- `helper(move borrowedParam)` where `helper` takes a `move` parameter is ALLOWED - the only
  call-site `move` guard (`MainListener.h:11553-11556`) fires on `move param->field`, not on
  bare-variable forwarding.

That is not a hole. The `move` at the call site **is** the programmer's assertion that the borrow
is dead and ownership is being handed on.

## Trap A - storing a BORROW into a structure that outlives the call

The callee stores a borrowed (non-`move`) pointer into a structure that outlives it. Ownership
was never transferred, so the producer still frees at scope exit.

```cflat
struct Node   { int v = 0; };
struct Holder { Node* slot = nullptr; };

// WRONG: `n` is borrowed - no `move` - yet it is stored into a structure that outlives us.
void store(Holder* h, Node* n)
{
    h->slot = n;
}

Holder* makeHolder()
{
    Holder* h = new Holder();
    Node* fresh = new Node();
    fresh->v = 42;
    store(h, fresh);     // ownership escapes into h->slot ...
    return h;            // ... but `fresh` is still owning here, and is freed.
}
```

Compiles clean; `h->slot->v` reads garbage. **Correct spelling:** `void store(Holder* h, move
Node* n)`.

Note this ONLY happens across a call boundary. Writing `h->slot = fresh;` directly in
`makeHolder` transfers ownership and is correct - see the table above.

### Trap A has now bitten three times, identically

Every time it was a B+tree split storing a freshly-allocated right sibling into the parent
through a **borrowed** parameter, and every time the tree corrupted at the first *internal-node*
split - around insertion #138-140 - because the leaf-split branch masks the identical mistake
(the leaf's `right` escapes earlier via `left->next = right`, so it looks fine until the tree
grows a level).

1. `core/hpc/btree.cb` - `_insertChildAt`'s `rightChild`.
2. `core/hpc/btree.cb` again, during the concurrency work.
3. `performance/perf_btree_scaling.cb` - an independent author writing a fresh benchmark-local
   tree from scratch made the *same* omission and lost hours bisecting it.

Three independent authors, same trap, same symptom, same insertion count. **The tell: a `new`
whose pointer is passed to a function that stores it.** If that parameter is not `move`, it is a
delayed use-after-free that will not surface until the structure grows a level.

## Trap B - `move` on a pointer copied OUT of a structure

`move` correctly nulls its source - verified for both locals and struct fields. But a plain
pointer copy does not, so an alias taken out of a field survives the move and dangles.

```cflat
void consume(move Node* n) { delete n; }

Holder h;
h.slot = new Node();
h.slot->v = 42;

Node* alias = h.slot;    // WRONG: plain copy. Both point at the node; h.slot is not nulled.
consume(move alias);     // frees it; `alias` is nulled, h.slot is NOT

// h.slot->v now reads garbage.
```

**Correct spelling:** `consume(move h.slot)` - moving the field directly nulls the field.

## Worked example: `core/hpc/btree.cb`

`remove()`'s rebalance walk is the reference implementation of the discipline:

| helper | node param | why |
|---|---|---|
| `_rebalanceFrom` | **borrowed** | the node stays live in the tree on most iterations; it only *conditionally* forwards to something that frees |
| `_mergeWithLeft` | **`move`** | unconditionally `delete`s the node on every call |
| `_mergeWithRight` | **`move`** (on `rightSib`) | unconditionally `delete`s the right sibling |
| `_collapseEmptyRoot` | **`move`** | only reached once the root is confirmed empty; the delete is unconditional |
| `_borrowFromLeft` / `_borrowFromRight` | borrowed | never free anything |
| `_insertChildAt` | **`move`** (on `rightChild`) | stores the new sibling into the tree - Trap A |

Verified: `test.sh` 170/0/15, `leaks --atExit` 0 leaks - neither a double-free nor a leak.

The concurrency work also found that the ownership model pushed toward the *correct* shape rather
than around it: `move n->values[i]` through a borrowed `n` is rejected, which forced splitting
`_leafInsert` into `_leafFind` + `_leafInsertNew` - and that split turned out to be required for
concurrency anyway, since the optimistic path must never free a value.
