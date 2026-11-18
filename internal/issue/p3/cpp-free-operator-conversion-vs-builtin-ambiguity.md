# Imported enum free operator resolution - leftovers

Free C++ operators on imported enums (binary, unary `~ - + !`, prefix `++`/`--` bound by CFlat `e++`)
rank against built-in arithmetic as clang does, enum+pointer operands included (ENUMPTR; legs 9800-9879 in Test/test_cpp_interop.cb, header
namespace `cppi_scope::eop` in Test/library/cpp_interop_scoped_enum.h). One case remains (item 2, needs a ruling).

## 2. File-scope int initializer folds the built-in

```cflat
import cpp "library/cpp_interop_scoped_enum.h";
int g = cppi_scope.eop.ua + 1;          // clang: 86 (operator+(UE, int)); CFlat: 6
const int gc = cppi_scope.eop.ua + 1;   // same
```

Master is the same. A file-scope initializer is folded natively before the overload path runs.
Fix direction: either evaluate a file-scope initializer that names an enum operator through the
call path (dynamic init), or refuse it like clang refuses it in a constant context.
