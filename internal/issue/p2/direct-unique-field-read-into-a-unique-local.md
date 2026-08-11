# A DIRECT `unique` field read assigned straight into a `unique` local

Filed 2026-08-10 on `fix/alias-provenance` round 2, from the review's probe
`scratch/rev_p5_directfieldbase.cb`. Measured IDENTICALLY on the merge base `ec3dff5` and on the
fix binary, so it is residue, not a regression - but the fix makes it conspicuous: the CALL
spelling of the same program now rejects while this one still double-frees.

```cflat
struct Res { int id = 0; ~Res() { dtorCount = dtorCount + 1; } };
struct PW { unique Res* p = nullptr; };
PW w; w.p = new Res();
unique Res* other = new Res();
other = w.p;              // accepted -> "id=5 dtor=1", then abort 133/134
```

Rejected twins, for contrast:

```cflat
Res* k = w.p;   unique Res* o = new Res(); o = k;    // REJECTED (borrow via a local)
o = w.getPlain();                                    // REJECTED (borrow via a call)
unique Res* o2 = w.p;                                // REJECTED (decl-init, borrowed value)
```

## Root cause

The unique-local `=` door (`MainListener_Expressions.cpp`, the `rightNV.IsBorrowed` arm inside the
`namedVar.TypeAndValue.IsUnique && Pointer` block) fires on a BORROW. A direct `unique` field read
carries `IsUniqueFieldAlias` / `TypeAndValue.IsUnique`, not `IsBorrowed` - the `=` path's Trap-B
block converts it, but only to RECORD the borrow on the destination binding afterwards, never to
reject the store that is happening. The intermediate-local and via-call spellings both arrive with
`IsBorrowed` already set and so hit the arm.

## Fix direction

Either widen that arm to accept `IsUniqueFieldRead(rightNV)` as a borrow, or - closer to the
2026-08-10 uniform-implicit-move ruling - make `unique` field -> `unique` LOCAL an implicit MOVE the
way field -> field already is, which needs the source field nulled and the destination's old value
released in that order. The second is the larger change and should be judged against that ruling
rather than bolted on here. Polarity unchanged: unknown accepts.

## Severity

A silent double free (compile 0, abort at run time, no diagnostic). P2 under the
residue-not-regression precedent.
