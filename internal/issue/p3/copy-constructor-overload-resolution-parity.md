# CFlat return into same-type initializer adds a copy

Summary: A CFlat-defined function returning a C++ class by value is copied into a same-type
initializer, where clang++ elides that copy.

Repro: `CV z = f(CV(7));`, with `CV` imported from a C++ header and CFlat defining
`CV f(CV a) { return CV(a.v); }`.

Measured on this worktree: value 7, k=1, b=3. clang++ -std=c++20: value 7, k=0, b=2.
Repro: `scratch/rev_copypar_repro/cf_ret_fresh/`.

Root cause direction: the initializer's C++ return-slot elision path is not armed for this
nested CFlat call expression, so the call result is materialized in a return temporary and then
copy-constructed into the declared object. Fix direction: route the CFlat call's return storage
directly to the initializer destination without consuming an outer slot for an inner call.
