Bucket: B (refused; C++ function-template argument spelling). Queue run B2 (parked 2026-09-28, restart from master on opus).

# std.min / std.max with `long` arithmetic or pointer-arithmetic arguments do not bind

Found 2026-09-27 by fix/free-wrapper-ref (pre-existing).
- `std.min(l - 10L, l)` with `long l` fails to bind (clang: std::min<long>).
- `std.min(p + 1, q)` with `int* p, q` is refused with "argument with no C++ type" (clang: std::min<int*>, `*result == 9`).

Root cause (B2, 2026-09-28): a declared `long` local spells `long`, but `l - 10L` reaches the implicit
argument typer (InferImplicitCxxArgumentType, LLVMBackend_CInterop.cpp) as bare `i64`, which
CxxSpellingForCflatType maps to `long long` (the `i64` entry beside `long`, ~4816). std::min<T> then sees
`long long` vs `long` and deduction fails. `p + 1` loses its pointee identity the same way.

What failed (patch scratch/b2_parked.patch, report scratch/b2p/b2_report.md): propagating a declared
source type name through EVERY expression result regressed `std.min(-ua, ub)` (int rvalue became `ptr*`,
leg test_cpp_interop_template.cb ~3349). Three narrower variants did not converge.

Direction: carry the operand's declared primitive name onto arithmetic results only when both operands
share it (or one is a literal) and the lowered type equals it; carry pointee identity only from a proven
pointer GEP. Never let a unary or scalar rvalue inherit a pointer identity.
