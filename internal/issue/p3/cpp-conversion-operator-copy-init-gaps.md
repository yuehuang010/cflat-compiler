# Copy-initialization through a C++ conversion operator: trivial targets fail, ctor-vs-operator ambiguity not reported

Two pre-existing gaps (master and the T35 branch), found by the T35 review 2026-10-03:

1. `T t = c;` where `C` has a non-explicit `operator T() const` and `T` is TRIVIALLY copyable fails with the
   bogus internal-looking "cannot cast an aggregate value"; the same shape with a non-trivial `T` binds
   through the clang copy-init wrapper.
2. `T t = mkC();` where both `T(const C&)` and `C::operator T() const` exist: clang reports the conversion
   as ambiguous; CFlat silently picks the constructor (prints 303).

## Repro

`scratch/repro_keep/t35_cvx/` (cvx.hpp + cv_r*.cb; the cases cv_rA etc. are case 1). Minimal shape:

```cpp
struct T { int v; };                       // trivially copyable
struct C { int v; operator T() const { return T{v + 1}; } };
inline C mkC() { return C{10}; }
```

```cflat
import cpp "cvx.hpp";
extern int main() { T t = mkC(); return t.v - 11; }   // expected 0 (clang); actual: cannot cast an aggregate value
```

## Fix direction

1. Route trivially copyable targets through the same clang copy-init probe as non-trivial ones (the trivial
   fast path must not pre-empt a user-defined conversion).
2. Let clang's copy-init answer decide: when clang reports ambiguity, LogError with the ambiguity, never
   pick the constructor.
