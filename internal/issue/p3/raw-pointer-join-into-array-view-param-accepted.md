# A raw-pointer `?:` join is accepted as an array-view parameter

Bucket: batch mode (one axis of one guard; freeze the accept-set first). Filed 2026-09-05 by the
review of fix/ptrview-param (master 83c404c); pre-existing, measured identical before and after
that commit. A false ACCEPTANCE, not a miscompile.

## Summary

A conditional expression joining two raw pointers can be passed to an array-view
parameter. This is a pre-existing acceptance hole, identical before and after the
pointer fixed-array view fix. It is intentionally not fixed in this commit.

## Repro

Probe (filed 2026-09-05 from the review of fix/ptrview-param, master 83c404c):

```cflat
int revTakeIntView(int[] v) { return 2; }

extern int main() {
    long a = 1; long b = 2; long* left = &a; long* right = &b;
    bool choose = a == b;
    return revTakeIntView(choose ? left : right);
}
```

Measured with the Release binaries:

- PRE (master `83c404c` Release binary):
  `build_rc=0`, `run_rc=2`.
- POST (the fix/ptrview-param binary): `build_rc=0`, `run_rc=2`.

Neither run emitted a diagnostic. The PRE and POST results are identical.

## Root cause

At `cflat/LLVMBackend_Overloads.cpp:851`, the raw-pointer rejection requires
`argTV.Pointer && !argTV.IsArrayView`. The `?:` pointer join reaches this gate
without carrying the raw-pointer `Pointer` fact, so the guard does not reject it;
the later element check also has no named source element to compare.

## Fix direction

First freeze the accept-set below, then preserve positive raw-pointer provenance and
the joined source element through `?:` lowering. Reject a join only when both the
pointer shape and the incompatible array-view binding are proven. Keep unknown
provenance conservative until the accept-set is covered by tests.

## Accept-set to freeze

- `int*[2]` -> `int*[]` fixed pointer-element array remains accepted.
- A declared `int*[]` view remains accepted.
- `int[2]` -> `int[]` remains accepted.
- A raw `int*` local -> `int[]` remains rejected.
- A raw `long*` `?:` join -> `int[]` must become rejected when the hole is fixed.
- Null/default joins with a compatible view remain accepted.
- Pointer joins passed to non-array-view pointer parameters remain unchanged.
