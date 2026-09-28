# C++ constructor with a `std::nullptr_t` parameter refuses a `nullptr` argument

## Summary

`Np(std::nullptr_t)` next to `Np(const long&)` refuses `cppcr.Np(nullptr)` with
"has no constructor whose parameter types match these arguments ('')": the `nullptr`
argument reaches the listed-constructor pick with an empty type name and no candidate
maps `std::nullptr_t`. Pre-existing on master (reviewer probe rev5 M27); C++ picks
`std::nullptr_t` (chosen 1).

## Repro

```cpp
// np.h
#include <cstddef>
namespace r5 { struct Np { int chosen; Np(std::nullptr_t) : chosen(1) {} Np(const long& x) : chosen(2) {} }; }
```

```cflat
import cpp "np.h";
extern int main() { return r5.Np(nullptr).chosen == 1 ? 0 : 1; }
```

Expected: exit 0. Actual: compile error, `('')` argument spelling.

## Root cause

The argument type of a bare `nullptr` is blank at the ctor call site
(`MainListener_PostfixExpression.cpp` C++ ctor path), so `SelectCxxConstructor` has
nothing to match and the clang-resolved thunk is not tried (the argument is not a
scalar reference case). `std::nullptr_t` has no CFlat spelling in
`CxxSpellingForCflatType` either.

## Fix direction

Spell a bare `nullptr` argument as `std::nullptr_t` (or `decltype(nullptr)`) for the
generated wrapper and let the clang-resolved thunk take it; `long` lvalues keep
`const long&` (chosen 2). Value leg in `Test/test_cpp_interop.cb`.
