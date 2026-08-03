# `simd<T,N>[] v = a;` fails module verification with no located diagnostic

**Severity: P3 - no miscompile and no false accept; the program IS rejected. The defect is that it
is rejected by the LLVM verifier instead of by a `LogError` with a source location, which
CLAUDE.md's debugging-workflow rule forbids.**

## Summary

Binding an ARRAY VIEW to a fixed array of simd vectors - `simd<T,N>[] v = a;` where
`a` is a `simd<T,N>[N]` - emits an invalid `ptr` -> `float` bitcast and dies in module
verification with a bare dump and no `file(line,col):` prefix. The equivalent plain-array
spelling (`float[2] a; float[] v = a;`) compiles cleanly.

`RecordSimdPointerAndDims` (landed on `fix/simdptr`) deliberately records only the pointer depth
and the FIXED dimensions for a simd declaration and leaves the `[]` array-VIEW spelling alone, so
the view decl path never learns the element is a vector and lowers the bind against the lane type.

This is DISTINCT from `internal/issue/p2/simd-type-spelling-unusable-outside-declarations.md`,
which is about the simd type spelling not being accepted in non-declaration positions. Here the
spelling parses fine; the lowering of the bind is what breaks.

## Measured repro (macOS arm64, Release)

`scratch/r4_simd_array_view.cb`:

```cflat
import "runtime.cb";

extern int main()
{
    simd<float,4>[2] a = default;
    simd<float,4>[] v = a;
    return 0;
}
```

`x64/Release/cflat scratch/r4_simd_array_view.cb -i Test/library --check` (verbatim, rc 1; the
`-o` path prints the same three lines followed by `Compilation failed.`):

```
Module verification failed:
Invalid bitcast
  %0 = bitcast ptr %arrptr to float

Error: module verification failed.
FAIL: scratch/r4_simd_array_view.cb
Checked 1 file(s), 1 failed.
```

## Provenance - unchanged by the amend, but NOT reachable before the branch

Measured, not inferred:

- HEAD of `fix/simdptr` and its pre-amend commit behave identically (rc 1, same dump).
- On `master` (`f463e7f`) the same file exits 0 and runs - but only vacuously: pre-branch the
  declarator's `[2]` was DROPPED, so `a` was a SINGLE vector and there was no array to view. The
  shape this issue is about could not be expressed at all. So no working program regressed; the
  branch made a new shape reachable and it lands on an unlocated verifier failure.
- The failure needs a real simd ARRAY source. `simd<float,4> a = default; simd<float,4>[] v = a;`
  (bare, non-array source) exits 0 on both `master` and HEAD.

## Fix direction

Locate the declaration path that binds a `T[]` array view and give it the simd element type for a
`simd<T,N>[N]` source - i.e. teach the view-bind the vector element the way
`RecordSimdPointerAndDims` teaches the fixed-dimension path. Failing that (view-of-vector may be a
feature nobody wants), emit a `LogErrorContext` at the declaration naming
`simd<T,N>[]` as unsupported, so the shape gets a located message instead of tripping the verifier.
Either way the verifier must not be the thing that reports it.
