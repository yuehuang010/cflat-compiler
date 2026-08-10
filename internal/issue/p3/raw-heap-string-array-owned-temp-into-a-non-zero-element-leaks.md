# An owned string temp stored into element k > 0 of a raw `new string[n]` array leaks

Filed 2026-08-10 by `fix/rawheap`. Pre-existing and UNCHANGED by that branch (measured on both
binaries); recorded because that branch closed every other cell of the same area.

Severity: silent leak, 16 bytes per element. No crash.

## Repro

```cflat
extern int main()                       // scratch/rh_30_tempnoread
{
    string* h = new string[2];
    h[1] = "ab" + "cd";                 // an OWNED temp; the slot adopts its buffer
    printf("v=%d\n", h[1] == "abcd" ? 1 : 0);
    return 0;                           // rc 0, `leaks --atExit`: 1 leak for 16 total leaked bytes
}
```

`leaks --atExit` reports 1 leak / 16 bytes on `b220d54` and on `fix/rawheap`. Element **0** does
not leak, because the scope-exit cleanup of an owning `string*` local runs `string.dtor` on the
FIRST element (and only that one) before `operator delete` - an asymmetry that is itself worth
knowing, and that contradicts the explicit `delete[n]` spelling, which the compiler rejects
outright for a raw `string*` local.

## Why it was not fixed with the rest

The natural fix is to reject an ownership-transferring source at that store. It cannot be gated
there: the destination/source signature of `h[k] = "a" + "b"` in user code is IDENTICAL to
`newData[i] = move _data[i]` inside `list<string>._grow` - both are a single-index GEP whose base
is a load from an alloca-backed local `string*`, with a source carrying no Storage, no Constant,
and no `IsMove` flag (measured with a temporary predicate dump on both). Rejecting the first
rejects the second and breaks `list<string>` growth. A fix needs real provenance on the
destination local (user `new string[n]` vs container scratch buffer), not a shape test.
