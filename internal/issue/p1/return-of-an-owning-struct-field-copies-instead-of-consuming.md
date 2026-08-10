# `return w.b` of an owning-struct FIELD copies it, so the caller double-frees

Filed 2026-08-09 by the `fix/bvfield` round, which found it while enumerating the store spellings
of a field consume. NOT the borrowed-by-value-parameter bug that round fixed: this one reproduces
for a LOCAL source and for a `move` parameter source too, so its root cause is separate.

Severity: double free (abort, rc 133).

## Repro

```cflat
int dtor = 0;
struct Res { int id = 0; ~Res() { dtor = dtor + 1; } };
struct UBox { unique Res* item = nullptr; };
struct Wrap { UBox b; };
UBox umk(int n) { UBox b; b.item = new Res(); b.item->id = n; return b; }

UBox mk2() { Wrap w2; w2.b = umk(3); return w2.b; }          // rc 133 (scratch/bv_c1)
UBox mk3(move Wrap w) { return w.b; }                        // rc 133 (scratch/bv_c2)
UBox mk4(Wrap w) { return w.b; }                             // rc 133 (scratch/bv_07)

extern int main() { { UBox r = mk2(); printf("v=%d\n", r.item->id); } return 0; }
```

Measured on `6c2302c` and on `fix/bvfield`: every one prints its value, then aborts. `mk2` prints
`v=2` because the second allocation's dtor already ran.

## Root cause

The store arms consume an owning field source (`ClassifyOwningAssignSource` -> Move -> zero the
source GEP) at six sites; the RETURN path has no such arm. `return w.b` loads the field's bits into
the return slot and leaves the field intact, so the returned value and the still-live `Wrap` both
own the same `Res`. The two guards in `MainListener_Statements.cpp` that do look at an owning field
return (~721, ~736) are gated on `currentFunctionReturnTV.IsMove`, so a plain `UBox` return type
reaches neither.

`return <bare local>` has an implicit-move arm (`movableLocalReturn`, ~491) and works correctly;
that arm keys on the return expression being a BARE IDENTIFIER, so a field path never enters it.

## Fix direction

Give the return path the same consume decision the store arms take: when the returned expression is
a field path whose type is a non-copyable owning value, MOVE it (zero the source field) rather than
copy. Then the borrowed-by-value-parameter guard added by `fix/bvfield`
(`RejectConsumeOfBorrowedByValueParamField`) has to run at that new site too, since `mk4` above is
exactly the shape that guard rejects everywhere else - it is currently the one consume spelling of
`w.b` that still compiles.

## Sibling spelling: the ARRAY ELEMENT return (measured 2026-08-10, `fix/viewelem`)

The same gap covers `return arr[0]` of an owning-struct element, in BOTH array spellings:

```cflat
Box take(Box[] v)  { return v[0]; }                                  // rc 133 (scratch/ve_r13)
Box take2() { Box[2] base; base[0] = makeBox(1); return base[0]; }   // rc 133, q=3 (scratch/ve_r13b)
```

Both are rc 133 on `0cfd9f7` AND on `fix/viewelem`; the second also hands back a WRONG value
(`q=4` as measured, but it is freed-memory garbage - do not treat the number as an oracle),
because the frame destroys the array on the way out. The `fix/viewelem` round left them alone
deliberately: the fixed-array spelling is the oracle for the view spelling and it fails the same
way, so this is the return path's missing consume arm, not the view-provenance bug that round
fixed. The element source has no `NamedVariable` name to `MarkVariableMoved`, so the new arm must
consume it silently, exactly as the element STORE arms already do for an indirect lvalue.

