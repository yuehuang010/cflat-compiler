# Implicit converting constructor refuses a `new T[n]` source the spelled form accepts

`a = new int[3];` into a C++ class with `NX(const int*)` is refused, while `a = NX(new int[3]);`
binds `NX(const int*)`. Violates "as if the user wrote `T(source)`"
(internal/plan/converting-constructors.md). Found 2026-09-27 reviewing fix/convctor-cpp (probe
rev3_nx); no speculation involved - a phase-1 resolver gap.

## Repro

```cpp
// nx.h
#pragma once
namespace nx { struct NX { const int* p = nullptr; NX() = default; NX(const int* q) : p(q) {} }; }
```

```cflat
import cpp "nx.h";
extern int main()
{
    nx.NX a = default;
    a = new int[3];            // refused; want NX(const int*) + move-assign
    return a.p != nullptr ? 0 : 1;
}
```

## Fix direction

The raw-array result view decay (`rawArrayResults_`, `FindRawArrayResult`, used at
MainListener_Expressions.cpp ~1453 / ~2694 for the spelled call) is not applied on the implicit
converting-constructor path; apply the same decay before classifying the source. Ownership: the
spelled form's ownership outcome is the reference (measure it with `leaks --atExit`).
