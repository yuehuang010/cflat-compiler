# Assigning into a `string` ELEMENT of a fixed array double-frees

Filed 2026-08-09 by `fix/arrslot` (the owning-STRUCT fixed-array-element fix), which measured
this cell and deliberately left it out: `string` ownership is a RUNTIME owned bit with its own
machinery, and the 2026-08-09 `fix/owncopy` record records what happens when the struct arm is
allowed to preempt it (a silent leak regression that every suite stayed green through).

Severity: double free (abort).

## Repro

```cflat
extern int main()
{
    string t = "hel" + "lo";          // heap-owned, not a literal
    string[2] dst; dst[0] = "ol" + "d";
    dst[0] = t;                        // rc 133
    return 0;
}
```

-> compiles 0, runs **rc 133**, and `leaks --atExit` reports 1 leak / 16 bytes (the old element).
Measured identical on `7beb979` and on `fix/arrslot`. A STRING-LITERAL source (`dst[0] = "x"`)
is clean on both, because neither side owns a buffer - that is why the cell is easy to miss.

## Root cause

Same destination-shape gap `fix/arrslot` closed for owning structs: a fixed-array subscript is a
TWO-index GEP over `[N x T]`, so it is neither `destIsStructField` nor `destIsLocalOwningVar`,
and the two `string` arms in `MainListener_Expressions.cpp` that would apply are gated on exactly
those two shapes - the field arm (`RejectOwningValueCopyIntoField` / the owned-string deep copy)
and the "destruct the old value of an owning-string LOCAL" arm (alloca/global only). Part 6's
container-slot arm DOES handle `slotElemType == "string"`, but only for a SINGLE-index GEP, and
it deliberately does not drop-old.

`fix/arrslot`'s new element arms exclude `TypeName == "string"` on both sides for this reason.

## Fix direction

Give the two-index array-element destination the same pair of string arms the whole-local
destination has: destruct the old element (the `string` dtor is null/owned-bit guarded, so a
never-assigned or borrowed slot is a safe no-op), then deep-copy an owned named source
(`EmitOwnedStringDeepCopy`) exactly as the field arm does. Do NOT route it through
`ClassifyOwningAssignSource` - that is the struct path, and the `fix/owncopy` record has the
measurement showing what it costs.

Note the owning-STRUCT-with-a-string-field spelling (`struct SBox { string s; }`, `SBox[2]`) is
already FIXED by `fix/arrslot` and is covered by the `aels_stringowner_*` legs in
`Test/test_move.cb`; only a bare `string` element is left.
