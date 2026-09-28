# std.min / std.max with `long` arithmetic or pointer arguments do not bind

Found 2026-09-27 by fix/free-wrapper-ref (pre-existing, same on its pre-fix binary).
- `std.min(l - 10L, l)` with `long l` fails to bind (clang: std::min<long>).
- `std.min(p + 1, q)` with `int* p, q` is refused with "argument with no C++ type" (clang: std::min<int*>).
Likely the argument-typing step for a C++ function-template call (InferImplicitCxxArgumentType /
TypeUntypedCtorArg family) has no C++ spelling for a `long` arithmetic rvalue or a pointer-arithmetic
result. Check what spelling `l - 10L` gets vs `l`.
