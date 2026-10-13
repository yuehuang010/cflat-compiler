# Imported C function-pointer typedef with a variadic signature is unsupported

From T145 review 1 (2026-10-12), pre-existing on master. `typedef int (*VP)(int n, ...);` in an imported C header, then a
CFlat use of `VP` -> "cannot find type VP"; clang accepts and calls through it (returns 35 in the review probe).
cflat/LLVMBackend_CInterop.cpp ~1866 explicitly rejects variadic pointer signatures, so the typedef is dropped.
Fix direction: bind variadic function-pointer types (call sites then follow the variadic rules incl. the T144/T145 aggregate
refusal), or keep the type and refuse at the use with a specific message instead of "cannot find type".
Repro: scratch/repro_keep/t145/ (copy of the review probe).
