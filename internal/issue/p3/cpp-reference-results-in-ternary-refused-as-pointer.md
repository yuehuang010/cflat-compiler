# A '?:' of two C++ reference results does not bind a C++ pointer

Found 2026-09-23 while fixing the reference-return-at-return issue (macOS arm64, Release). Pre-existing.

## Summary

`C* p = c ? ref_a() : ref_b();` is refused with "cannot initialize pointer 'p' with a value of
type 'C'", and `return c ? ref_a() : ref_b();` from a `C*` function hits the return backstop
"cannot return this value: its type does not match". Each arm alone binds as a borrowed pointer
at a declaration, an assignment and (since fix/cpp-reference-return-at-return) a return.

## Repro

scratch corpus `rr_p07.cb` / `rr_q11.cb` (header: two `static` cells returned by `C&`).

## Root cause (hypothesis, not measured)

The '?:' lowering loads each alias arm and joins VALUES, so the join is not an alias value and
`CxxReferenceResultAsPointer` (which requires `IsAliasValue(src.Storage)`) has no address to bind.

## Fix direction

When both arms are C++ reference results of the same (or base-convertible) class and the
destination is a pointer, join the arm ADDRESSES (base-adjusted per arm).
