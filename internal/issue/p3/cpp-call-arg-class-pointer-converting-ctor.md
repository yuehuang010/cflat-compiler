# C++ class pointer call argument not converted through a converting ctor

Found by the T304 Opus review (pre-existing on master, still refused after T304).

## Repro
```cpp
struct Foo { int z = 3; };
struct R { int v; R(const Foo* p) : v(p->z) {} };
inline int take_r(R r) { return 110 + r.v; }
```
CFlat: `Foo foo = default; take_r(&foo)` -> "no overload of 'take_r'". clang++ -std=c++20 accepts (113).

## Root cause
`ClassifyCxxImplicitArgument` (cflat/LLVMBackend_CInterop.cpp ~29994) early-outs `classPointerArg` before converting-ctor
selection. T304 added `NamedVariable::AllowCxxClassPointerCtorConversion`, set only at the three operator operand sites
(MainListener_Expressions.cpp ~16479/16501/17274); ordinary call arguments keep it false.

## Fix direction
Set the opt-in for C++ call arguments (free + member) whose pointee is a proven C++ record and whose parameter is a C++
class; keep CFlat struct pointers, Foo**, void*, unrelated Bar* refused. Oracle: clang value AND chosen overload, incl.
exact `const Foo*` overload beating the conversion and Derived* -> R(const Base*).
