# Bare `list<T*>` element with a live owner elsewhere: silent use-after-free / double free

Filed 2026-07-19; narrowed 2026-08-21 to the one OPEN case. The closed sub-cases (direct
`delete l.get(0)`, `list<unique T*>` borrows, reassignment clearing the borrow tag, the opt-in
`list<alias T*>` spelling) are recorded in the digest in `internal/fix-issue-lessons.md` and are
not to be re-investigated.

## Repro

```cflat
class Node { int v = 0; double pad[8] = default; };
extern int main(int argc, char** argv) {
    list<Node*> xs;                  // borrows; frees nothing
    int i = 0;
    while (i < 4) {
        Node* n = new Node;          // owning local: freed at the end of THIS iteration
        n->v = i * 10;
        xs.add(n);                   // no diagnostic
        i++;
    }
    i = 0;
    while (i < xs.count()) { Node* n = xs.get(i); printf("xs[%d].v = %d\n", i, n->v); i++; }
    return 0;
}
```

Measured on `39d4b38`, Release: compiles clean, prints `xs[k].v = 5` for every element (ASan:
heap-use-after-free). No `delete` is needed to trigger it; the same shape with
`B* o = xs.get(0); delete o;` is the double-free variant.

## Root cause

`list<T*>` never frees its elements, so whether an element is still valid (and whether deleting it
is a double free) depends only on whether some OTHER owner still holds the object - decided at the
`add` site, possibly in another function. The two programs below are identical at the `get`/`delete`
site; only the first is wrong:

```cflat
list<B*> l; B* p = new B(); l.add(p);  B* o = l.get(0); delete o;   // E: p still owns -> double free
list<B*> l; l.add(new B());            B* o = l.get(0); delete o;   // F: sole handle -> legal idiom
```

F is the blessed "borrowing container + manual free" idiom used by the suites and the core UI
framework, so it must stay legal.

## Ruled out (do not re-spike)

- **Blanket reject** of any delete of a `get()` borrow: breaks F (`test.sh` 464->458,
  `example_mac` 35->27, `test_lsp` 152->135).
- **Owner-linked taint at the add site** (tag the container when `add` receives a live owning
  local, propagate to `get()` views, reject their delete): distinguishes E from F and stays green,
  but is intra-procedural. Split the add into `Bag.put(p)` and the delete into `Bag.delFirst()` and
  it compiles clean and double-frees at runtime. Maintainer ruling: a check that only catches the
  same-function shape is worse than none (false coverage). Reverted.

## Ruling (maintainer, 2026-08-21)

Bare `list<T*>` is NEITHER owning nor borrowing - it carries no ownership policy, and the CALLER
is responsible for correct ownership. `B* p = new B();` is an owning (secretly unique) local, so
handing it to a container that will not free it and then letting it die is the caller's error, and
the caller resolves it with `move`:

```cflat
list<B*> l; B* p = new B();
l.add(p);        // ERROR: 'p' still owns the object and is freed at scope exit; add(move p)
l.add(move p);   // OK: p is nulled; the element is now the sole handle (manual-free idiom F)
l.add(new B());  // OK: rvalue, already F
```

Do NOT make bare `list<T*>` owning (rejecting borrowed arguments would break every
borrow-collection on `list<T*>` / `dictionary<K,T*>`); borrows of objects owned elsewhere stay
legal, and `list<alias T*>` remains the opt-in checkable spelling for them.

## Fix direction

Add-site rule, decidable locally: when `add` / `set` / `insert` (any element-storing method) of a
non-owning container receives a LIVE OWNING named local (owning pointer, not `unique`, not a
borrowed parameter, not already moved) by borrow, reject with a diagnostic naming `move`. This is
the same add-site test rejected approach B used, but it rejects the ADD itself instead of tainting
the container for a later delete, so there is nothing to propagate across functions and no false
coverage. Polarity: unknown provenance ACCEPTS.

Known, accepted gap: `Bag.put(p)` where `put` does `list.add(param)` - inside `put` the argument is a
borrowed parameter, so it is accepted and still dangles when the caller's `p` dies. Not false
coverage (the rule is stated as "owning local needs move"), but file it as a residual when this
lands. Legs: the loop repro above must be rejected; idiom F, `add(move p)`, adding a borrowed
parameter, adding a `unique` element with `move`, and the core UI borrow-collections must stay
accepted (`test.sh`, `example_mac.sh`, `test_lsp.sh` green).

Queue after [[string-field-read-from-container-element-requires-copy]] lands - same guard files.

## Related

- `internal/plan/unique-ownership.md` - the `alias` design.
- [[temp-unique-field-escapes-through-an-indirect-callee-or-an-unfollowable-return]] - same
  provenance gap, field flavour.
