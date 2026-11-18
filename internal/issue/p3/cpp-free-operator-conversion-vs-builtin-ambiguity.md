# Imported enum free operator resolution - leftovers

Free C++ operators on imported enums (binary, unary `~ - + !`, prefix `++`/`--` bound by CFlat `e++`)
rank against built-in arithmetic as clang does (legs 9800-9864 in Test/test_cpp_interop.cb, header
namespace `cppi_scope::eop` in Test/library/cpp_interop_scoped_enum.h). Two cases remain.

## 2. File-scope int initializer folds the built-in

```cflat
import cpp "library/cpp_interop_scoped_enum.h";
int g = cppi_scope.eop.ua + 1;          // clang: 86 (operator+(UE, int)); CFlat: 6
const int gc = cppi_scope.eop.ua + 1;   // same
```

Master is the same. A file-scope initializer is folded natively before the overload path runs.
Fix direction: either evaluate a file-scope initializer that names an enum operator through the
call path (dynamic init), or refuse it like clang refuses it in a constant context.

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
