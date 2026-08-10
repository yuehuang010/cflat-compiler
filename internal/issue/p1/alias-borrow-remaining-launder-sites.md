# An alias-borrowed POINTER into an owning pointer local

Filed 2026-08-07 by `fix/aliaslaunder` as one of three neighbouring shapes left open by the
five-site borrow predicate. **Cells 1 and 3 landed on `fix/aliasres` (2026-08-10)** - re-binding the
borrow local itself (a destination-side drop-old suppression plus a block-scoped retirement of the
borrow classification) and `move` of a FIELD of a borrow local (a rejection mirroring the
borrowed-by-value-parameter twin, with the by-reference lambda-capture carve-out). This cell is what
remains: it is blocked on separating the two sanctioned meanings of an `alias T*` return, which
neither of those changes touched. Its measurement below is UNCHANGED by them - re-verified against
the `fix/aliasres` binary on 2026-08-10 (compiles, prints `id=5 dtor=1`, aborts).

Common prelude (identical to the closed issue's):

```cflat
int dtorCount = 0;
struct Res { int id = 0; ~Res() { dtorCount = dtorCount + 1; } };
struct Box { unique Res* item = nullptr; };
struct Wrap { Box b; alias Box get() { return this.b; } };
Box makeBox(int id) { Box x; x.item = new Res(); x.item->id = id; return x; }
```

## An alias-borrowed POINTER into an owning pointer local

```cflat
struct PWrap { unique Res* p = nullptr; alias Res* get() { return this.p; } };
PWrap w; w.p = new Res(); w.p->id = 5;
Res* k = w.get();
unique Res* other = new Res();
other = k;             // compiles; run 133 after printing "id=5 dtor=1"
```

The landed reject gates its DESTINATION on an owning VALUE type, so a `unique T*` destination is
untouched. Widening it there was measured too risky to do blind: `alias T*` has a second, sanctioned
meaning documented in `BindingKeepsOwnershipOfBoxedObject` (`MainListener.h`) - `alias T* e =
makeT(); IS s = e; delete s;` is the CORRECT hand-off idiom - so a source-only rule would
false-reject it. Closing this needs the two meanings of an `alias` pointer return separated first.

Cell 1's fix does not reach it either: the destination here is a `unique T*` POINTER local, not an
`alias`-borrow binding, so `DestinationIsAliasBorrowLocal` answers false and the pointer store path
is untouched by design.

## Severity

A silent double free (compile 0, abort at run time, no diagnostic). Filed P2 rather than P1 under the
residue-not-regression precedent: it is accepted by the PRE binary (`86f929b`) exactly as it is by
the fix, so it is residue of the closed issue's family, not anything the fix introduced.
