# `h - &k` never offers the address-of operand to a C++ operator's class parameter

A member `operator-(ConvIp)` where `ConvIp(const int*)` converts an `int*` LOCAL operand
(`h - ip`) but not the address-of expression `h - &k`: "no overload of 'operator-' matches".
The call form `h.add(&k)` works. Found 2026-09-27 fixing
cpp-pointer-argument-never-offered-to-class-parameter.

## Repro

```cpp
// oa.h
#pragma once
namespace oa { struct P { int v = 0; P(const int* p) : v(*p) {} };
struct H { int base = 1; H() {} int operator-(P p) const { return base - p.v; } }; }
```

```cflat
import cpp "oa.h";
extern int main()
{
    int k = 7;
    oa.H h;
    return (h - &k) == -6 ? 0 : 1;   // refused; `int* ip = &k; h - ip` works
}
```

## Root cause guess

TryBinaryOperatorOverload (MainListener_Expressions.cpp) rebuilds the right operand from a raw
llvm::Value; `Pointer` is stamped only from a recorded `rhsPointerDepth >= 1`, and `&k` records
no depth (unary `&` never invents one), so the operand arrives as a bare `ptr` with no pointer
flag and no pointee name. Carry the call-argument proof (operand slot type) or the pointee name
across for the address-of shape.
