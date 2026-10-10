# Brace-initialized C++ ctor with enum-lvalue elements picks the wrong overload

Found by T58 review 3 (pre-existing, identical on master).

## Repro (scratch/repro_keep/t58_rev/rev3/)
```cpp
enum E { e0, e1 };
struct A { A(const int (&)[2]); A(const unsigned (&)[2]); };
struct L { L(std::initializer_list<int>); L(std::initializer_list<long>); };
```
```cflat
E e = E.e1;   // CFlat spelling of the unscoped enum lvalue
A({e, e});    // cflat: unsigned[2] ctor; clang++ -std=c++20: int[2] ctor (enum promotes to int)
L({e, e});    // cflat: "ambiguous"; clang: initializer_list<int>
```

## Fix direction
The brace selector must spell an unscoped-enum element by its promoted type (int, or the
underlying type for fixed-underlying enums per [conv.prom]) - same identity path T58 added for
arithmetic results (CxxArithIdentity / brace selector ~LLVMBackend_CInterop.cpp:9116).
