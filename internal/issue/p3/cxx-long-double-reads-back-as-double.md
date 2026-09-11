# A C/C++ `long double` binds inbound as `double` and loses its C++ identity

Found 2026-09-10 while confirming primitive coverage after the C/C++ primitive-typing change.
Small identity gap, not an ABI bug.

## Repro

```cpp
// inbound.hpp, imported with `import cpp "inbound.hpp";`
namespace pr { inline long double r_ldouble() { return 0; } }
```

`typeof(pr.r_ldouble())` prints `double`. A C header's `long double` binds the same way.

## Why it is only an identity gap

`MapCTypeToTypeAndValue` (`cflat/LLVMBackend_CInterop.cpp`, the `base == "long double"` row)
maps `long double` to `double` only when `IsCInteropLongDoubleSupported()` holds - a 64-bit IEEE
`long double` (Windows, macOS arm64). Every other target (x86-64 Linux: 80-bit) refuses the
binding with `CInteropLongDoubleRefusal()`, so no call is ever made with a mismatched ABI.

What is lost is identity: CFlat's own `long double` (`longdouble`) is a C++ template argument
only, so a value that came in as C++ `long double` cannot be re-spelled as one. Passing it back
into a template instantiates `N` (`double`), not `O` (`long double`).

## Fix direction

Needs a maintainer ruling first: either keep `long double` inbound as `double` (document it; the
value is bit-identical where it binds), or give `longdouble` a native CFlat identity on targets
where it is 64-bit IEEE, so it can flow back into C++ unchanged.

## Acceptance

- The ruling is recorded here or in `internal/fix-issue-lessons.md`.
- If identity is kept: a C++ function returning `long double`, fed into `std.vector<...>` via
  `typeof`, instantiates `O` on win64.
