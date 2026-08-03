# The `simd<T,N>` type spelling is not accepted outside a declaration specifier

Filed 2026-08-03 while fixing `simd-pointer-declaration-aborts-the-compiler` (that abort IS
fixed; these are the cells the coverage matrix turned up that the abort was hiding). All four
behave IDENTICALLY on `904f026` and on `fix/simdptr` - measured per spelling, not inferred.

Severity: hard error or silent drop. No crash, no silent wrong VALUE in the first three.

## Repro - measured with `-o` on Release builds of `904f026` AND `fix/simdptr`

```cflat
// (a) C-style cast target
extern int main(){ simd<float,4> v = 8.0f; simd<float,4>* p = (simd<float,4>*)&v; return 0; }
// simd_cast.cb(1,79): unknown type 'simd<float,4>'          // both binaries, rc 1

// (b) lambda parameter
extern int main(){ auto f = (simd<float,4>* p) => { return (*p)[0]; }; return 0; }
// simd_lam.cb(1,52): unknown type 'simd<float,4>'           // both binaries, rc 1

// (c) function<> / tuple signature component
extern int main(){ (simd<float,4>*, int) t = default; return 0; }
// type 'tuple__simd<float,4>ptr__i32' has an incomplete layout   // both binaries, rc 1

// (d) array VIEW - silently dropped, compiles as a plain vector local
extern int main(){ simd<float,4>[] a; printf("view\n"); return 0; }
// rc 0, prints "view"                                       // both binaries

// (e) POINTER TO ARRAY - neither spelling reaches the guard that rejects it for every
//     other element type, and the second one's message is not true of the source.
extern int main(){ simd<float,4>[]* p;  return 0; }   // rc 0, silently accepted
extern int main(){ simd<float,4>[2]* p; return 0; }   // rc 1: "simd<T,N> supports the
                                                      // static calls '.load'/'.store'."
// controls, same binary:
//   float[]*  -> "pointer to array-view 'float[]*' is not a valid type"
//   float[2]* -> "pointer to fixed array 'float[N]*' is not a valid type; pass 'float*'"
```

## Root cause - partly diagnosed

`simd<T,N>` is a builtin special form recognised only in `ParseDeclarationSpecifiers` (both
copies) and, since the load/store bridge, as a `primaryExpression` via `simdTypeSpecifier`
(`CFlat.g4`). Every other type position - `typeName` in a cast, `functionPointerParam`, a tuple
type argument, a lambda parameter - resolves a type by NAME, and there is no registered type
named `simd<float,4>`, hence "unknown type". This is the same class of gap `function<R(P)>`
had before `EncodeClosureCodegen`: the special form needs an encoded name to travel through
name-keyed positions.

(d) is different: `RecordSimdPointerAndDims` (`MainListener.h`) deliberately leaves an EMPTY
`[]` alone on a simd type, because a simd array VIEW is unimplemented and deducing one there
would change a shape that currently compiles. The `[N]` and `**` forms WERE closed by
`fix/simdptr`; only the view form still drops silently.

(e) is the pointer-to-array guard. It runs in the mainstream declarator tail, which the simd
branch `break`s out of before reaching, so `simd<T,N>[]*` is accepted and `simd<T,N>[2]*` falls
all the way out of declaration parsing and is re-read as the EXPRESSION `simd<float,4>[2] * p`
(the same mis-parse that makes the bare `simd<float,4>* sp;` message an unreliable oracle - see
the `fix/simdptr` landed record). It gains weight now that `fix/simdptr` made `simd<T,N>[N]` real
array storage: the shape the guard protects against actually exists for simd today.

## Fix direction

(a)-(c) want one mechanism, not four patches: give `simd<T,N>` an encoded, registerable type
name (mirroring `BuildEncodedClosureName`) and resolve it wherever a type is looked up by name.
Do NOT special-case the cast path alone - the same hole reappears at every name-keyed position,
which is exactly how the closure-encoding work went.

(d) is a separate decision: either implement `simd<T,N>[]` as a real array view (it lowers to a
thin `ptr` like any other view, and the element stride is well-defined) or REJECT the empty
bracket on a simd type with a located diagnostic. Silently dropping it is the one answer that
is wrong.

(e) wants the simd branch to reach the same pointer-to-array guard every other type reaches,
rather than a fourth copy of it. Do it together with (d): both are the same structural cause -
the simd branch leaves the specifier loop early - and `RecordSimdPointerAndDims` is where the
facts are already local.
