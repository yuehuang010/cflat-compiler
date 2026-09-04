# Returning a scalar from a simd<T,N> function errors instead of splatting

## Summary

`simd<float,4> f() { float x = 1; return x; }` - assignment of a scalar into `simd<T,N>` storage
splats it across the lanes, but `return` of the same scalar does not. On master (cb3f71b) this was a
silent module-verifier dump; the narrow-promotion branch added a backstop in
`LLVMBackend::CreateReturnCall` (cflat/LLVMBackend_MoveDataflow.cpp ~1565) so it is now the
diagnostic "cannot return this value: its type does not match the declared return type of function
'f'". Strictly better than the crash, but inconsistent with assignment.

## Fix direction

Ruling: should `return` splat a scalar into a vector return type the way assignment does? If yes,
route the return operand through the same splat helper `CreateAssignment` uses before the integer
narrowing arm, add a leg in the existing simd test. If no, keep the diagnostic and note the
asymmetry in doc/LANGUAGE.md's simd section.
