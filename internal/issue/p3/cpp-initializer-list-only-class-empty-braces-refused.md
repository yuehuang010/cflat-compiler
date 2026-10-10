# `T v{};` on a C++ class whose only ctor takes std::initializer_list is refused

Found by T63 review 1 (pre-existing on master).

## Repro (scratch/repro_keep/t63_rev/)
```cpp
struct IL { int n; IL(std::initializer_list<int> l) : n((int)l.size()) {} };
```
```cflat
cpp.IL v{};   // cflat: refused, no default constructor
```
clang++ -std=c++20: `IL v{};` value-initializes via the initializer_list ctor with an empty list
([dcl.init.list]/3: empty braces + default ctor -> value-init; no default ctor -> overload resolution
over initializer_list ctors). n == 0.

## Fix direction
Empty-brace declaration path: when the class has no default ctor, route through the T63 brace
thunk (`new (slot) T{}`) and let clang choose.
