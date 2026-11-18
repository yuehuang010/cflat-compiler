# Imported enum free operator resolution - leftovers

Binary free C++ operators on imported enums now rank against built-in arithmetic as clang does
(legs 9800-9823 and 9825-9826 in Test/test_cpp_interop.cb, header namespace `cppi_scope::eop` in
Test/library/cpp_interop_scoped_enum.h). Four cases remain.

## 1. Unary free operators on enums use the built-in

Repro (header already in tree):

```cpp
// add to cppi_scope::eop in Test/library/cpp_interop_scoped_enum.h
inline int operator~(SE e) { return 500 + (int)e; }
inline int operator-(UE e) { return 600 + (int)e; }
```

```cflat
import cpp "library/cpp_interop_scoped_enum.h";
extern int main()
{
    int a = ~cppi_scope.eop.SE.a;   // clang: 505
    int b = -cppi_scope.eop.ua;     // clang: 605
    return a == 505 && b == 605 ? 0 : 1;
}
```

CFlat runs the built-in for both. Root cause: `TryUnaryFreeOperatorOverload`
(cflat/MainListener_BinaryOperators.cpp) has the same struct-only gate the binary `tryFreeOperator`
had. Fix direction: pass the operand's resolved imported-enum identity (`ResolveEnumTypeName`,
checked against `importedEnumTypes_`) into the unary path at each call site, and send a clang
request only when a free operator whose parameter resolves to that enum exists; skip while
`nativeConstantFoldDepth_ > 0`.

## 2. File-scope int initializer folds the built-in

```cflat
import cpp "library/cpp_interop_scoped_enum.h";
int g = cppi_scope.eop.ua + 1;          // clang: 86 (operator+(UE, int)); CFlat: 6
const int gc = cppi_scope.eop.ua + 1;   // same
```

Master is the same. A file-scope initializer is folded natively before the overload path runs.
Fix direction: either evaluate a file-scope initializer that names an enum operator through the
call path (dynamic init), or refuse it like clang refuses it in a constant context.

## 3. A clang refusal falls back silently to the built-in

When clang rejects the operator request (for example two equally good enum overloads, ambiguous),
the enum branch in `tryFreeOperator` falls back to built-in arithmetic with no diagnostic. clang
reports the ambiguity as an error. Fix direction: relay the clang refusal (prefixed "clang: ", per
the clang-prefix ruling) instead of falling back, once a candidate naming the enum was found.

## 4. Enum plus pointer operand overloads

`operator+(E, int*)`, `operator+(E, C*)`, and `operator+(E, in::S*)` are never selected; imported
enum plus pointer uses master's built-in pointer arithmetic or refusal. For example, clang returns
1 below while master CFlat returns 0:

```cpp
#pragma once
namespace ep { enum E : int { zero = 0 }; inline int* operator+(E, int* p) { return p + 1; } }
```

```cflat
import cpp "enum_pointer.h";
extern int main() { int[2] a = {0, 0}; int* q = ep.zero + &a[0]; return q - &a[0]; }
```

When a scoped enum pointer is passed through the free-function wrapper, its parameter is spelled
`T* &` (`RequestCxxFreeFunctionImpl`, pointer lvalue classification near line 4721), so a failed
probe must fall through silently to GEP. Deleted or ambiguous enum-pointer overloads must error
like clang instead of silently taking built-in pointer arithmetic.
