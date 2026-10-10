# Brace-initialized global C++ class refused with a misleading message

Found by T63 review 2 (pre-existing, master same).

## Repro
```cflat
std.vector<int> gv{1, 2, 3};   // file scope
```
cflat: "positional initializers are not supported" (misleading - locals support them since T63).
clang++ -std=c++20: valid (dynamic initialization at startup).

## Fix direction
Either route through the T63 brace thunk in the global initializer function (respecting the global
storage ruling: no exit-time destruction), or refuse with a specific message naming the global
C++ class case.
