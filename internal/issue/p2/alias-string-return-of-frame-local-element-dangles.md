# `alias string f()` returning a frame-local array element dangles, undiagnosed

Filed 2026-08-09 from the fix/strread review. Pre-existing: identical on `bfb5943` and on
fix/strread (`6b6e3ec`), whose return arm deliberately excludes `alias` functions and neither
causes nor worsens this.

Severity: unconditional dangle - rc 133 / wrong value at the caller - from a spelling nothing
diagnoses.

## Repro

```cflat
alias string f() {
    string[2] dst;
    dst[0] = "ab" + "cd";
    return dst[0];       // borrow of a frame-local buffer the frame then destroys
}
extern int main() {
    string r = f();
    printf("r=%d\n", r == "abcd" ? 1 : 0);   // rc 133 / r=0 on both binaries
    return 0;
}
```

## Root cause

An `alias` return is a borrow by design, but the borrowed storage here is the function's own
frame (a fixed-array element whose teardown runs at scope exit). Returning it is always a dangle.
The return-dangle analysis (PointsIntoStackFrame, used by the statloc fix) does not cover the
alias-string-element shape.

## Fix direction

Diagnose at compile time: in the `alias` return path, when the returned string borrows storage
that PointsIntoStackFrame (the element GEP roots at a frame alloca), reject with a located error
naming the local. This is a rejection of an always-wrong program, not a behaviour change to
working code. Check the field spelling (`alias string f() { B b; ...; return b.s; }`) and the
whole-local spelling in the same pass.
