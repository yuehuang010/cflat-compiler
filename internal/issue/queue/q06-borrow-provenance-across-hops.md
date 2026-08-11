# q06: Borrow/alias provenance lost across a hop

7 items. A fact is correctly recorded on a local (borrowed root, view element, decay provenance)
and then silently dropped when the value crosses a field access, a parameter, or an indirect call.
Downstream guards see a plain pointer and either free the owner's memory or say nothing.

## Shared root cause

Provenance facts live on `NamedVariable` / `TypeAndValue` as single flags with no propagation rule.
Every hop is a separate site that must remember to copy the flag, and most do not. Guards then test
one specific flag (`IsBorrowed`, `RootIsBorrowedByValueParam`) rather than "is this reachable from
something I do not own".

## Members

- `p1/temp-unique-field-escapes-through-an-indirect-callee-or-an-unfollowable-return` - only the
  `function<T>` half is open; see the note below. NOT a straightforward member of this bucket.
- `p2/view-bound-from-borrowed-param-field-escapes-consume-guard` - the binding carries
  `IsViewElement` but not the root-of-borrowed-param fact.
- `p2/implicit-consume-of-a-borrowed-parameters-field-has-no-diagnostic` - guard exists for
  explicit `move` only.
- `p2/implicit-consume-of-a-field-of-a-borrow-local-double-frees` - guard checks
  `RootIsBorrowedByValueParam` but not the alias-borrow-local twin.
- `p2/store-into-a-field-of-a-borrow-local-drops-the-owners-value` - field drop-old destructs
  unconditionally.
- `p2/string-pointer-param-slot-semantics-depend-on-argument-provenance` - decay provenance is
  tracked on locals only; a parameter cannot carry caller intent.
- `p2/delete-borrow-via-named-local` - bare `list<T*>` cannot distinguish a borrowed element from
  an owning rvalue element at the delete site.

## Fix direction

1. Replace the flag pair with a single "root provenance" answer: given a value, walk back through
   field GEPs, view bindings, and parameter slots to the ROOT and report owned / borrowed / unknown.
   Every consume, delete, and drop-old guard then asks that one question.
2. Make the root walk cross a parameter boundary by encoding provenance in the parameter slot
   (this is what `p2/string-pointer-param-slot-semantics...` needs).
3. For the indirect-callee case, see the ruling below - do NOT make `unknown` reject.

Do after q01. Overlaps q05's guards; expect to touch the same functions, so serialize the two.

## Ruling on `p1/temp-unique-field-escapes...` (corrected 2026-08-11)

An earlier draft of this file summarized it as "indirect (funcptr/interface) callees have no callee
analysis" and proposed making `unknown` reject. Both are wrong.

**Stale.** The interface-dispatch half was CLOSED 2026-08-10 by `fix/iface-escape` (judged against
the closed implementor set, ALL-of-implementors in both directions), and the below-call-site return
half by `fix/retlate`. Read the issue file before planning anything here; it is a running record of
six sub-cases, most now closed.

**Ratified polarity: unknown ACCEPTS.** Same governing rule as q09. A `function<T>` value has no
closed world short of a points-to analysis, so the remaining funcptr shape is recorded as a
deliberate ACCEPT cell, not an unfixed bug:

```cflat
function<void(Node*)> f = keep;   // void keep(Node* n) { g = n; }
f(makeBox().t);                   // measured: dtors=1, dangles - accepted by design
```

**Already-tried-and-rejected.** Making `ParameterMayReachReturn` follow a load through a GEP (to
catch `Node* viaField(Node* n) { Slot s; s.q = n; return s.q; }`) was BUILT and MEASURED, and it
false-rejects `int readResourceId(Resource* r) { return r->id; }` in `Test/test_move.cb` itself.
The type-based refinement (follow only a load of POINTER type) false-rejects
`Node* getNext(Node* n) { return n->next; }`. Closing this needs field/offset awareness the walk
does not have. Do not retry either.

**Do not consolidate.** The issue file says so explicitly: its remaining sub-cases are separate
mechanisms sharing one guard, and the two that were fixed were each fixed alone. It is filed in
this bucket for adjacency only - it does NOT share the flag-propagation root cause the other six
members share, and it should not be swept into a single "root provenance" change.

Residues still open and deliberately accepting: a GENERIC interface base clause (implementor set
not enumerable before substitution), an interface-dispatch call whose ARGUMENT is a candidate
launder, and two budget escapes (`kMaxRetainDepth`, `kMaxRetainUses`).
