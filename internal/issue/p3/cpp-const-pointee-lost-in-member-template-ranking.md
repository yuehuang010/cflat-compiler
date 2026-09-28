# Pointee const is lost when a C++ pointer argument reaches clang overload ranking

Found 2026-09-27 (AQ round-2 review, fix/mt-nontpl). Every member template beside a non-template
overload now goes through clang, which exposes that CFlat drops pointee const on C++ pointer results.

## Repro

A C++ member returning `const T*&`, `const T* const&`, `T const* const&`, `const T* const*`, or a plain
by-value `const T*`, then passed to an overload set that distinguishes `const T*` from `T*`:
`h.cpr()->g(sh)` gives 102 (clang 2); `k.kind(h.cpr())` gives 1 (clang 2). `T* const&` (top-level const
only) is correct. Master matched clang for the by-value shape only because it never asked clang.

## Root cause

The C++ -> CFlat type mapping drops pointee const (2026-08-26 "const unenforced" ruling); the argument
type handed to the generated clang wrapper is then `T*`. `IsCxxConstRef` also means two things on member
returns (set for `T*const&`, CInterop.cpp ~14421).

## Fix direction

Carry C++ pointee const internally on the TypeAndValue that came from a C++ call (the 2026-09-27 ruling:
CFlat does not expose const but respects C++ const internally) and spell it in the wrapper parameter.
Fix all spellings together, including by-value `const T*`. Split the `IsCxxConstRef` meaning.
Round-trip any new field through the --init cache.
