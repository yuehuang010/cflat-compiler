# Call with a defaulted trailing parameter leaks a copy of a C++ class argument

## Summary

Calling a CFlat function that has a defaulted trailing parameter, with a C++ class prvalue for the by-value parameter, makes
an extra copy of the argument that is never destroyed. clang constructs the parameter in place and destroys it once.
Master 6f5ed3b2b (found by the MIXCOND3 review, 2026-10-30).

## Repro

scratch/repro_keep/defargcopy/ (main checkout): x_defplain.cb + x_defplain.cpp + h.h (logging ctor/copy/dtor class B).

```cflat
int de(B v, int e = 3) { lg('['); return v.get() + e; }
int probe(bool k) { return de(mkB(20)); }
```

CFlat trace `bp[y live=1` (b = make, p = copy, y = one dtor; the copy is never destroyed). clang: `b[y live=0`.
Without the defaulted parameter (`take(mkB(20))`) the trace equals clang, so the default-argument call path forwards a copy.

## Fix direction

Find where the call with appended default arguments re-materializes the explicit argument (likely a forwarding/wrapper path
for defaulted parameters) and pass the original prvalue slot through, so the parameter is constructed in place and destroyed
once. Add a logging-copy class leg; the MIXCOND S2 shape (`t513V10Def(k ? mkA() : mkB())`) is refused until this lands.
