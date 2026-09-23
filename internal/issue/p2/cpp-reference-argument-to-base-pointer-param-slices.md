# A C++ reference result passed to a CFlat base-pointer parameter is copied and sliced

Found 2026-09-23 while fixing the reference-return-at-return issue (macOS arm64, Release). Pre-existing,
silent wrong value.

## Summary

`int takeR(R* r)`; `takeR(bref())` where `B& bref()` and `struct B : L, R` copy-constructs a
temporary `R` (through R's copy constructor, fed B's UNADJUSTED address) and passes the
temporary's address: the callee reads L's field and a write through `r` is lost. The same-class
form (`takeC(cell())` into `C*`) passes the referent's address, as the issue that introduced
`CxxReferenceResultAsPointer` intended.

## Repro

scratch corpus `rr_a11.cb`: exits 33 (L's lv), expected 44.

## Fix direction

A reference result into a POINTER parameter of a base class is a borrow of the referent: pass the
base-adjusted address (as `CxxReferenceResultAsPointer` does at declaration / `=` / return), never
the by-value base-copy conversion.
