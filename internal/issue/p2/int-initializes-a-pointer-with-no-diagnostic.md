# An `int` initializes a pointer with no diagnostic, producing a wild pointer

Found 2026-09-16 by the C++-interop bug bash round 3 (macOS arm64, Release, worktree at master 2798eb1a).
NOT C++-interop-specific - reached through a C++ member returning `int&`, then reduced to plain CFlat.

## Summary

`int* a = x;` where `x` is an `int` compiles clean and stores the integer VALUE as the pointer.
Dereferencing it crashes. C rejects this without a cast; CFlat rejects the analogous class case
(`bb3c.C* mr = bb3c.mut_ref();`) but lets the primitive through.

## Repro

`scratch/bb3_intptr.cb` (no imports):

```cflat
extern int printf(const char* f, ...);
int five() { return 5; }
extern int main()
{
    int x = 5;
    int* a = x;
    int* b = five();
    printf("a=%p b=%p\n", a, b);
    return 0;
}
```

Measured: compile exit 0, run exit 0, output `a=0x5 b=0x5`.

How it was reached (`scratch/bb3_const4.cb`): a C++ member `int& slot()` behaves as an lvalue int
(reading gives 5, `c.slot() = 42` writes through - both correct), so `int* s = c.slot();` silently
becomes the pointer `0x5` and `*s = 42` segfaults (run exit 139) on a clean compile.

## Fix direction

Reject an integer initializer / assignment for a pointer-typed target unless it is a literal 0 or an
explicit cast, the same way the class case is already rejected. The C++-interop symptom disappears
with it.

Suggested bucket: p2 (missing diagnostic that turns a plausible typo into a wild pointer; core
language, so blast radius should be measured against the test suite first).
