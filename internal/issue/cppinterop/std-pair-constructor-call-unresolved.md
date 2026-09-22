# `std.pair<A, B>(a, b)` does not resolve as a constructor call

Split 2026-09-22 from the constrained-template-constructor issue (its "name does not resolve"
family). Of that family, `std.optional<int>(7)`, `std.regex("a+")` and `std.mt19937(1u)` already
compile and run on master d59df39b; only `pair` remains, and for a different reason than the
constructor-template ranking fixed there.

## Repro

```cflat
import cpp "utility";
extern int main() { std.pair<int, double> p = std.pair<int, double>(1, 2.5); return p.first; }
```

    'pair$int$double' is not a member of namespace 'std' (C++ free function 'std.pair$int$double'
    could not be bound (clang: no member named 'pair$int$double' in namespace 'std'))

`std.pair<int, double> p = default;` and a pair returned from C++ both work.

## Root cause (measured)

`LLVMBackend::IsForeignCxxClassWithConstructors` (cflat/LLVMBackend.h) opens with
`if (typeName.starts_with("std.pair$")) return false;`, so neither the declaration path nor the
postfix `T(args)` path treats `std.pair<...>` as a constructor class. The call falls through to
the C++ FREE-function fallback, which spells the mangled CFlat name `pair$int$double` into C++.
`std.pair$` is also special-cased as a CFlat value record (`CClangExtract.cpp` pairValue,
`LLVMBackend_CInterop.cpp` stdValueRecord). The exclusion came in with 86c9befb without a stated
reason; find what it protects before removing it.

## Fix direction

Remove or narrow the exclusion so `T(args)` reaches the constructor path (every pair constructor
is a template, so it goes through the clang-resolved wrapper), and run the suite for whatever the
exclusion protected. Acceptance: the repro returns 1 and `p.second == 2.5`, asserted in
Test/test_cpp_interop_template.cb next to the constructor-template section (2540-2559).
