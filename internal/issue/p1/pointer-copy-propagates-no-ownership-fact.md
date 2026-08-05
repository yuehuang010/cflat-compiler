# A plain pointer COPY propagates no ownership fact (four silent double frees)

Filed 2026-08-04 by the round-1 review of `fix/untracked-copy`, which closed ONE member of this
family - a copy of an owning LOCAL, keyed on the source binding's own `IsOwning` - and measured
these four while building its accept set. All four are pre-existing, all four are IDENTICAL on
`312d202` and on the merged fix, and none is boxing- or delete-guard-specific.

## Root cause

A pointer DECLARATION whose initializer reads another binding copies the VALUE and nothing else.
Each ownership fact a source binding can carry is propagated by its own hand-written clause at the
decl-init site in `MainListener.h` (`srcIsBorrowed`, `srcBorrowsOwnedElement`, `srcIsOwningMove`,
and now `srcBorrowsOwningLocal`), so a fact with no clause simply vanishes across the copy. The
copy then looks like a binding with no obligations at all, and every downstream guard - the raw
`delete` guard, `BindingKeepsOwnershipOfBoxedObject`, the store-into-`unique`-field check - is
consulted on a binding whose provenance was erased one line earlier.

The already-closed member is the shape of the fix: record the fact at the DECLARATION, where both
sides are in hand, and re-ask it at the consumer. Each repro below needs its own clause and its own
accept set, which is why they were not folded into that change.

## Repros

Common prelude for all four (`scratch/upc/_prelude.txt` on the fix branch):

```cflat
int dtorCount = 0;
interface IS { int area(); };
class Ci : IS { int r = 7; int area() { return r; } ~Ci() { dtorCount = dtorCount + 1; } };
class HoldU { unique Ci* h = default; };
bool idb(bool b) { return b; }
```

### (a) A copy stored into a `unique` field - rc 134 on both binaries

```cflat
HoldU hh;
Ci* c = new Ci();
Ci* b = c;
hh.h = b;          // accepted; the field's synthesized destructor and `c` both free it
```

The store path rejects a `IsBorrowed` source (`RejectBorrowIntoUniqueLocal`, the same guard that
rejects `unique Ci* b = c;` at a declaration - measured rejected). The copy carries no such flag,
so the store is accepted and the object is freed twice. This is the STORE half of the guard the
fix closed for `delete`; `BorrowsOwningLocal` is deliberately not read there yet, because the store
path has its own accept set (a copy whose source is later rebound, a copy of a late-assigned local)
that was never enumerated.

### (b) A one-hop copy of a container-element borrow - rc 134 on both binaries

```cflat
list<unique Ci*> l;
l.add(new Ci());
Ci* e = l.get(0);  // `e` carries BorrowsOwnedElement
Ci* b = e;         // `b` carries nothing
delete b;          // accepted; the list's destructor frees it again
```

`delete e;` on the direct spelling IS rejected ("it borrows an element that 'l' owns"). The
decl-init clause reads `TypeAndValue.IsBorrowOfUniqueElement` off the accessor RESULT, never
`NamedVariable::BorrowsOwnedElement` off a source BINDING, so exactly one hop defeats it. Closing
it means propagating `BorrowsOwnedElement` across the copy, whose retirement rule already exists
and is subtle (`SetVariableBorrowsOwnedElement` refreshes on `=`, `??=` suppresses it via
`CoalesceRebound` - see [[interface-boxing-keyed-on-source-binding]]).

### (c) `move` off a copy adopts ownership the copy never had - rc 134 on both binaries

```cflat
Ci* c = new Ci();
Ci* b = c;
Ci* d = move b;    // `d` adopts; `c` still frees at scope exit
```

`move` of a BORROWED source is already rejected into a `unique` destination
(`MainListener.h`'s `srcIsBorrowed && ... typeAndValue.IsUnique` gate). A plain `Ci*` destination is
not covered, and the copy is not a recognised borrow anyway. Note the fix branch's `delete b;`
twin of this program IS rejected now, so the raw-delete and `move` spellings of the same mistake
disagree.

### (d) A `?:` join into a POINTER declaration - rc 134 on both binaries

```cflat
Ci* c = new Ci();
Ci* b = idb(true) ? c : c;
delete b;          // accepted, 134 - and the boxed twin `IS s = b; delete s;` is 134 too
```

The DIRECT boxed spelling `IS s = idb(true) ? c : c; delete s;` is correctly REJECTED on both
binaries ("it boxes an object that 'c' already frees"), because the join is boxed per arm and each
arm's binding is still in hand there. Routing the same join through a pointer local first erases
it: a `?:` result carries no source binding, so no decl-init clause fires. This is the join axis of
the same missing propagation, and it is the reason the closed member's guard is keyed on a plain
read of a live binding rather than on the declaration's syntax.

## Fix direction

One clause per fact, at the decl-init site, mirroring `srcBorrowsOwningLocal`: record by STORAGE
IDENTITY, carry the origin for the diagnostic, and re-ask liveness at the consumer so the fact
retires when either end is rebound. Build the accept set FIRST for each - (a) and (b) in particular
have shapes where the copy is the SOLE owner (a late-assigned source, a rebound source) and a
rejection there would leak, the polarity error this family has paid for twice. (d) needs the join
rule the `??`/`?:` boxing work already settled: both arms must prove, or the fact is dropped.

## Related

[[interface-boxing-keyed-on-source-binding]] - the six-clause boxing proof and its retirement rules.
The closed member of this family is the fix/untracked-copy design record in
[[interface-issue-queue]].
