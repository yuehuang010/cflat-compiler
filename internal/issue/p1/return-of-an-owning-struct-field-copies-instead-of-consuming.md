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
