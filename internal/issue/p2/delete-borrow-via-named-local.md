# `delete` of a borrowed named local - opt-in spelling closes it; bare case still open

Filed 2026-07-19. Three narrow double-free forms are FIXED (below). The general bare-pointer case
(1a) is still OPEN, but there is now an OPT-IN mitigation: `list<alias T*>` is a distinct pure-borrow
spelling whose accessor results reject `delete`. Two earlier compile-time errors that tried to close
1a for BARE `list<T*>` without annotation were both rejected (history in git); the `alias` spelling
sidesteps them by making the borrow intent part of the type.

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

## Mitigated by an opt-in spelling: `list<alias T*>` (added 2026-07-21, SPIKE - kept, uncommitted)

`alias` was revived as a generic type ARGUMENT (it had been banned as redundant). `list<alias T*>` is
a DISTINCT type from `list<T*>` - the qualifier is carried as a leading `"alias "` prefix through
monomorphization (mangles `alias_`, so `list__alias_B_ptr` != `list__B_ptr`), exactly like `unique`.
It is a modifier ON the type (C++ `const int*` vs `int*`): dropping it is a different type.

Semantics: `list<alias T*>` BORROWS its elements identically to `list<T*>` inside the body (never
frees), but declares "these elements are owned elsewhere", so EVERY accessor result is a borrow the
caller must not free:

```cflat
list<alias B*> l; B* p = new B(); l.add(p);
B* o = l.get(0);  delete o;   // ERROR (laundered get/[] borrow)
B* o = l.take(0); delete o;   // ERROR (take removes the slot but never transferred ownership)
```
vs the unchanged manual-free spelling, where the same code is legal:
```cflat
list<B*> l; l.add(new B());
B* o = l.get(0); delete o;    // OK - manual-free idiom (F)
```

Mechanism (mirrors `unique`): the accessor return type carries `TypeAndValue.IsBorrowOfAliasElement`
(`--init` key `"bae"`), propagated to `NamedVariable.BorrowsOwnedElement` +
`BorrowedElementExternallyOwned` (message selector), rejected by the SAME delete check. The
derivation gate is `if (substAliasArg) ...` (NOT gated on the return being spelled `alias`), so it
covers `take()`'s plain-`T` return too - an `alias X*` value is a borrow no matter which accessor
produced it. Invariant: **`list<alias T*>` yields only non-deletable borrows from every accessor.**
Regressions: `Test/errors/err_delete_alias_borrow.cb` (get + take legs, cold and warm cache),
`err_alias_type_arg_rejected.cb` (repurposed: `alias` still requires a pointer/interface element),
plus a positive leg in `Test/test_list_ownership.cb`. Suites: `test.sh` 466/0/8, `example_mac` 35/0,
`test_lsp` green.

What this does NOT do: it is OPT-IN. A user who writes bare `list<B*>` for a borrow-of-a-live-owner
(the E case below) still gets no error. The `alias` spelling gives the borrow intent a checkable home;
it does not make bare `list<B*>` safe.

### Known residuals of the alias spike
- The delete diagnostic says "its container" rather than the container's variable name (the accessor
  result carries an empty container name - same limitation as the `unique`-element message).
- Shallower than C++ `const`: the qualifier lives on the CONTAINER instantiation, but accessors hand
  back a bare `B*`, so the borrow-ness is re-projected onto each accessor's return via the flag rather
  than riding the result's own type. That is why `take()` needed its own gate fix and `get()` did not
  cover it for free.
- Stale comments elsewhere still say the spelling was removed (`test_generics.cb:774`,
  `test_list_ownership.cb:158`); not yet refreshed.
- `queue<T>` / `dequeue()` are NOT yet aligned - see the open question below.

## Open - the general BARE-pointer double-free (1a, unannotated)

```cflat
list<B*> l; B* p = new B(); l.add(p);   // E: p is a LIVE owning local; list<B*> only BORROWS
B* o = l.get(0); delete o;              // double-free: delete o, then p auto-frees at scope exit
// vs
list<B*> l; l.add(new B());             // F: owning RVALUE, no named owner
B* o = l.get(0); delete o;              // o is the sole handle -> LEGAL (would leak without it)
```
Same `list<B*>`; the only difference is the `add` argument (named owner vs rvalue). E must be
rejected, F must stay legal. They are identical at the delete site, so distinguishing them needs
add-site / interprocedural provenance. The `alias` spelling above lets a user OPT IN to rejecting the
laundered delete, but for the bare spelling with no annotation both compile-time attempts were
rejected:

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
from F and kept all suites green, but was deliberately REVERTED because it is INTRA-PROCEDURAL and
gives FALSE COVERAGE: split the add into another function (`Bag.put(p)` / `Bag.delFirst()`) and it
compiles clean but DOUBLE-FREES at runtime (exit 134). Two reasons: (1) inside the split function `p`
is a borrowed PARAMETER, not a live owning local, so the add-site heuristic never fires; (2) the
container tag is per-function-body local state that does not persist across functions.

Maintainer ruling: a partial error that only catches the same-function shape is worse than none - it
lulls the caller into thinking the double-free is caught while the cross-function form still ships. A
real fix for the BARE case needs interprocedural escape/ownership analysis ("does any live owner still
hold this element?"), the same capability approach A also lacked. Deferred until that analysis exists;
do NOT reintroduce a local-scope-only error for the bare spelling. The `alias` opt-in is the
recommended path for callers who want the check today.

## Related
- `internal/plan/unique-ownership.md` - the `alias` design.
