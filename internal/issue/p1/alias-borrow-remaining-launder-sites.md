# An alias-borrowed POINTER into an owning pointer local

## RULING 2026-08-10 (maintainer) - COMPUTE the provenance. Unknown ACCEPTS.

Reject adoption only for a PROVEN borrow-of-live-owner. Anything unproven keeps compiling.

**The "two sanctioned meanings of an `alias T*` return" framing below is misleading, and it is what
made this look blocked.** `alias` has ONE meaning (`cflat/MainListener.h:3410`): this binding's
scope-exit free is SUPPRESSED - it frees nothing. What differs between the two cells is not the
keyword but **whether anything else owns the object**:

- `Res* k = w.get();` where `get()` returns a `unique` FIELD - the field owns it. `k` is a borrow of
  a LIVE owner, so adopting it into an owning destination creates a second owner. MUST reject.
- `alias T* e = makeT();` where `makeT()` returned an owned value - the alias binding dropped that
  ownership, so NOBODY owns it, and `IS s = e; delete s;` is the only way to release it. MUST
  accept, or it leaks.

"Does a nameable other owner exist?" is computable from the initializer at the alias binding site,
so **no language change is needed** and the two meanings never have to be separated. Record the
answer on the binding, then gate adoption on it.

`BindingKeepsOwnershipOfBoxedObject` already asks nearly this exact question next door, with three
positive proofs: the binding frees it itself (`IsOwning`); it reads a `unique` field; or it is a
non-`move` pointer parameter. It excludes `IsAliasBorrow` because it is answering the INVERSE
question, but the underlying fact is the same one - so that proof set is the thing to reuse, not a
reason this cannot be done.

**Residue accepted by the ruling:** an opaque or forward-declared alias-returning callee yields no
proof and keeps laundering. Consistent with the family's unknown-accepts polarity (a false rejection
is a blocker; a missed diagnostic is today's behaviour). Note cflat emits IR as it walks, so
"callee defined below the call site" is a common source of unknowns; a record-then-resolve pass at
end of module would shrink the residue if it proves to matter.

Follows the 2026-08-10 governing principle - "CFlat is a simplified reading: if ownership can be
computed, it should be implicit" - stated in full in
[[unique-field-to-field-array-element-receiver]].

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
