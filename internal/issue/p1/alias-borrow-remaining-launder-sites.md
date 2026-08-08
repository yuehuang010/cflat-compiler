# Three alias-borrow launder sites left open by the five-site predicate

Filed 2026-08-07 by `fix/aliaslaunder`, which closed the two persist sites named in
[[alias-borrow-local-launder-gaps]] (owning-local store, `move` argument) plus the return / field /
brace-init sites they were unified with. These three neighbouring shapes were MEASURED on the same
corpus, are the same family, and were deliberately left out - each needs a different question than
"is the SOURCE a borrow", which is the only question the landed predicate asks.

Common prelude (identical to the closed issue's):

```cflat
int dtorCount = 0;
struct Res { int id = 0; ~Res() { dtorCount = dtorCount + 1; } };
struct Box { unique Res* item = nullptr; };
struct Wrap { Box b; alias Box get() { return this.b; } };
Box makeBox(int id) { Box x; x.item = new Res(); x.item->id = id; return x; }
```

## 1. Re-binding the BORROW local itself destroys the borrowed old value (dest-side)

```cflat
Wrap w; w.b = makeBox(5);
Box k = w.get();
k = makeBox(2);        // compiles; run 133 after printing "id=2 dtor=1"
```

The owning-local reassignment path destructs the OLD destination unconditionally, and here the old
destination is a borrow of `w.b` - so `w` frees the same pointee again. The landed predicate asks
about the SOURCE (`makeBox(2)` is a genuine owner), so it does not fire. The correct behaviour is
probably not a rejection but a suppression: skip the drop-old when the destination binding is
`IsAliasBorrow`, and clear the flag so the local owns what it was just given. That is a
destination-side change with its own accept set, which is why it was not folded in here.

## 2. An alias-borrowed POINTER into an owning pointer local

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

## 3. `move` of a FIELD of a borrow local

```cflat
void psink(move Res* p) { }
Wrap w; w.b = makeBox(5);
Box k = w.get();
psink(move k.item);    // compiles; run 133 after printing "dtor=1"
```

`k` shallow-copies `w.b`, so nulling `k.item` leaves `w.b.item` live and both the sink and `w` free
the pointee. `ParseMoveExpression` already rejects the parameter twin of this
(`IsBorrowedStructParameter` on `ParentVariableName`); the local twin needs the same lookup extended
to a registered `IsAliasBorrow` binding, with the by-reference lambda-capture carve-out the landed
`BorrowAdoptionIsUnsound` uses (a by-ref capture's `Storage` IS the outer owner's address, so
consuming it really does move out of the one owner).

## Severity

All three are silent double frees (compile 0, abort at run time, no diagnostic). Filed P2 rather
than P1 under the residue-not-regression precedent: every one of them is accepted by the PRE binary
(`86f929b`) exactly as it is by the fix, so they are residue of the closed issue's family, not
anything the fix introduced.
