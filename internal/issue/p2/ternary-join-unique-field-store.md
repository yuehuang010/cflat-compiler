# A ternary JOIN of two `unique` field reads stored into a `unique` field

Split out of `p1/alias-borrow-remaining-launder-sites.md` on 2026-08-10, when that file's own cell
(an alias-borrowed POINTER adopted into an owning destination) was closed on `fix/alias-provenance`.
This cell is join-provenance work and was never part of it. Re-measured on the `fix/alias-provenance`
binary: UNCHANGED (compile rc 0, run rc 133/134 - double free).

Probe: `scratch/abp_ternary_join.cb`.

```cflat
int freed = 0;
struct Node { int v = 0; ~Node() { freed = freed + 1; } };
struct H { unique Node* slot = nullptr; };
H a = default; H b = default; H c = default;
a.slot = new Node(); a.slot->v = 4;
b.slot = new Node(); b.slot->v = 6;
c.slot = 1 == 1 ? a.slot : b.slot;   // accepted; `a.slot` NOT nulled
```
```
v=4 freed=0 anull=0     compile rc 0, run rc 133 (double free)
```

Measured IDENTICALLY on the merge base `c9405da`, so it PREDATES the uniform-implicit-move change
and is residue, not regression.

## Root cause

The join strips the field provenance: `IsUniqueFieldRead` answers false on the joined value, so the
store sees an ordinary pointer. The DIRECT spelling `c.slot = a.slot` is now an implicit move
(source nulled, one owner), so the two spellings disagree - which makes this shape MORE reachable
than before, since the direct form no longer rejects and a user reaching for the join gets no
diagnostic either.

## Fix direction

Have the join carry the field provenance (the same value-identity ledger the closed alias cell
used), after which the joined value takes the implicit move like any other unique field read.
Polarity unchanged: unknown accepts. The BOTH-ARMS quantifier applies - see `JoinArmsKeepOwner`,
which already proves "every non-null arm resolves to a live binding that keeps ownership".

## Severity

A silent double free (compile 0, abort at run time, no diagnostic). P2 under the
residue-not-regression precedent.
