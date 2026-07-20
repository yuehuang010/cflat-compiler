# `delete` of a borrowed named local - two remaining cases

Filed 2026-07-19. The two GUARANTEED-double-free forms are FIXED; two narrower forms remain.
This file tracks only what is still open. History (the direct-call fix, the reverted broad-taint
spike, the false-positive analysis) is in git.

## Fixed (do not re-investigate)

- Direct call result: `delete l.get(0)` / `delete l.take(0)` - fixed 2026-07-19.
- `unique`-element named local: `list<unique B*> l; B* g = l.get(0); delete g;` - fixed
  2026-07-20 by carrying element ownership on the accessor's monomorphized return type
  (`TypeAndValue.IsBorrowOfUniqueElement`, LLVMBackend.h; `--init` key `"bue"`), propagated to a
  dedicated `NamedVariable.BorrowsOwnedElement` consulted ONLY by the delete check, so no
  store/return/move false positives. Discriminator is "does the container own the element
  (unique)", not "came from an accessor", so `dictionary<int,int*>` views and `list<B*>` bare
  lists are correctly NOT flagged. Regression: `Test/errors/err_delete_borrowed_owned_element.cb`;
  all 300 `test_collection_leaks` subtests still pass.

## Still open

**1. Borrowed view, deleted after the ADD site still owns the pointee.**
```cflat
list<B*> l;                     // bare = borrowed view (add takes it as a borrow, does NOT null p)
B* p = new B(); l.add(p);       // p stays owning and non-null
B* o = l.get(0);                // o == p
delete o;                       // frees once here; then p's scope-exit auto-free -> SIGABRT
```
Verified reachable in current syntax (`scratch/docverify/item1now.cb`, exit 134, 2 dtors). The
element type is a bare `B*` (a borrow), not `unique B*`, so `IsBorrowOfUniqueElement` is not set -
correctly, because the CONTAINER does not own it. The double-free comes from the ORIGINAL `p`,
which still auto-frees at scope exit. A blanket "reject delete of a container borrow-read" is NOT
safe: the rvalue-add form `list<B*> l; l.add(new B()); B* o = l.get(0); delete o;` is CORRECT
(`scratch/docverify/rvaladd.cb`, exit 0 - `delete o` is the only free, since nothing else owns
it). The two are indistinguishable at the delete site; catching item 1 needs provenance from the
ADD site (that `l` borrows a pointer some named local still owns), not element ownership.
Different machinery from the fixed case; deferred.

**2. Reassignment does not clear the borrow tag.**
```cflat
B* g = l.get(0);  g = new B();  delete g;   // wrongly rejected - g now owns a fresh alloc
```
This MIRRORS the pre-existing borrowed-parameter delete rule, which rejects the identical shape
(`B* p = borrowedParam; p = new B(); delete p;`). It is a shared limitation of the borrow-taint
machinery, not new to the 2026-07-20 change. Flow-sensitivity would fix both at once; deferred.

## Related
- `internal/plan/unique-ownership.md` - the `alias` design.
