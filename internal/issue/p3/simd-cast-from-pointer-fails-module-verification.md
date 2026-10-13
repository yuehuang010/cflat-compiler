# Cast of a pointer to a simd vector type fails module verification

Summary: `(simd<float,4>)p` and `(simd<int,4>)p` with `int* p` reach LLVM module verification and
fail there instead of a CFlat diagnostic (master and T121 alike). T121 refuses scalar
pointer -> float casts at MainListener_Expressions.cpp ~15265 via `isFloatingPointTy()`.

Found: T121 review 1 (2026-10-11), pre-existing.

Fix direction: refuse pointer -> vector casts with a LogError: widen the T121 guard to
`isFPOrFPVectorTy()` for float vectors and add an integer-vector refusal (pointer -> integer needs an
explicit scalar cast first; check clang -std=c17 + ext_vector_type wording).
