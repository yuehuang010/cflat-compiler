# `delete` of a borrowed named local - one case open, two approaches rejected

Filed 2026-07-19. Three narrow double-free forms are FIXED (below). The general bare-pointer case
(1a) is OPEN: two different compile-time errors were built for it and BOTH were rejected - a blanket
reject (1b) breaks blessed idioms, and an owner-linked local check (1a-attempt) only covers the
intra-function case and was judged not good enough. History (the reverted spikes, the false-positive
analyses, and the free-function bisection that corrected the mental model) is in git.

## Fixed (do not re-investigate)

- Direct call result: `delete l.get(0)` / `delete l.take(0)` - fixed 2026-07-19.
- `unique`-element named local: `list<unique B*> l; B* g = l.get(0); delete g;` - fixed
  2026-07-20 by carrying element ownership on the accessor's monomorphized return type
  (`TypeAndValue.IsBorrowOfUniqueElement`, LLVMBackend.h; `--init` key `"bue"`), propagated to a
  dedicated `NamedVariable.BorrowsOwnedElement` consulted ONLY by the delete check. Discriminator is
  "does the container own the element (unique)", not "came from an accessor". Regression:
  `Test/errors/err_delete_borrowed_owned_element.cb`.
- Reassignment clears the borrow tag: `B* g = l.get(0); g = new B(); delete g;` - fixed 2026-07-20.

These three are sound because they need NO provenance: the direct form deletes a call-result borrow
(null Storage), and the unique form keys on element-TYPE ownership. Both are decidable at the delete
site alone.

## Open - the general bare-pointer double-free (1a)

```cflat
list<B*> l; B* p = new B(); l.add(p);   // E: p is a LIVE owning local; list<B*> only BORROWS
B* o = l.get(0); delete o;              // double-free: delete o, then p auto-frees at scope exit
// vs
list<B*> l; l.add(new B());             // F: owning RVALUE, no named owner
B* o = l.get(0); delete o;              // o is the sole handle -> LEGAL (would leak without it)
```
Same `list<B*>`; the only difference is the `add` argument (named owner vs rvalue). E must be
rejected, F must stay legal. They are identical at the delete site, so distinguishing them needs
add-site / interprocedural provenance. Both attempts to build a compile-time error were rejected:

### Rejected approach A - blanket reject (was called "1b"). SPIKED, NOT VIABLE, reverted 2026-07-20.
Reject any delete of a get()-borrow named local (or, equivalently, reject an owning rvalue handed to
a borrowing container). Breaks the blessed "borrowing-container + manual-free" idiom (an rvalue in a
`list<X*>`/`dictionary<K,X*>` freed later via `get()`+`delete`, or a `destroyTree()` walk): `test.sh`
464->458/6, `example_mac.sh` 35->27/8, `test_lsp.sh` 152->135/17. It fired on `test_collection_leaks.cb:1069`
(passes HeapAudit, provably not a leak) and the core UI framework. A call/delete-site rule cannot see
the DOWNSTREAM free, so it cannot tell E from F. Do not re-spike as a local rule.

### Rejected approach B - owner-linked local taint. SPIKED VIABLE, but REVERTED 2026-07-20 as not good enough.
Tag the container variable at the add site when `add`/`set` receives a LIVE owning named local
through a borrowing param; propagate to `get()` views; reject their delete. This DID distinguish E
from F: it caught E, left F/rvalue/call-result/unique/move idioms legal, and all three suites stayed
green (464/0/8, 35/0, 152/0) with +147 lines and no `--init` change. It was implemented, verified,
then deliberately REVERTED because it is INTRA-PROCEDURAL and gives FALSE COVERAGE:

- Split the add into another function and the check misses the bug entirely - it compiles clean and
  DOUBLE-FREES at runtime (exit 134). Verified:
  ```cflat
  class Bag { list<B*> l = default; void put(B* p) { l.add(p); } void delFirst() { B* o = l.get(0); delete o; } };
  Bag bag; B* p = new B(); bag.put(p); bag.delFirst();   // compiles; 134 at runtime
  ```
  Two independent reasons: (1) inside `put`, `p` is a borrowed PARAMETER, not a live owning local, so
  the add-site heuristic never fires; (2) the container tag is per-function-body local state that does
  not persist from `put`'s analysis into `delFirst`'s.
- Also coarse even intra-function: the taint is per-container-VARIABLE, so a container that MIXES a
  named-owner add and rvalue adds over-taints all its views (over-rejects; no suite code hit it).

Maintainer ruling: a partial error that only catches the same-function shape is worse than none - it
lulls the caller into thinking the double-free is caught while the cross-function form still ships. So
the local-scope error was reverted. A real fix needs interprocedural escape/ownership analysis ("does
any live owner still hold this element?"), the same capability approach A also lacked. Deferred until
that analysis exists; do NOT reintroduce a local-scope-only error for it.

## Related
- `internal/plan/unique-ownership.md` - the `alias` design.
