# C++ class converting ctor: global initializers refused

CONVREST (2026-11-19) fixed the mixed-pointee `?:` (composite pointer type, derived arm adjusted
before the PHI) and the `T**` source ([conv.qual]) of cpp-class-pointer-converting-ctor-remaining-sources.
One design item remains. Measured 2026-11-19 on the CONVREST exe (cold = warm).

Repro header (rv.h):

```cpp
struct RvFoo { int value = 3; };
struct RvR { int value; RvR(const RvFoo* p) : value(p ? p->value : 0) {} };
struct RvI { int value; RvI(int i) : value(i) {} };
struct RvPP { int value; RvPP(const RvFoo* const* p) : value(9) {} };
```

1. **Global converting initializer:** `RvFoo ga = default; RvR gr = &ga;` (clang 3), `RvI gi = 5;`
   (clang 5), `RvFoo* gfp = &ga; RvPP gq = &gfp;` (clang 9) are refused ("cannot initialize value of
   type 'RvR' with a pointer of type 'RvFoo*'" / "from a single scalar value"). Root cause: a CFlat
   global's initializer is evaluated in the throwaway `__global_init_tmp` function
   (MainListener_Declarations.cpp, `globalInitTempFn`) and must fold to a compile-time constant;
   there is no dynamic initializer for a CFlat-declared global. Even the spelled form
   `RvR gr = RvR(&ga);` is refused ("has an initializer that is not a compile-time constant").
   HDRGLOB's path initializes HARVESTED C++ header globals through clang and does not cover a
   CFlat declaration. Fix direction: a CFlat global dynamic-initializer path (ordered like C++
   dynamic initialization) - a design question for the global/static storage ruling, not a
   conversion gap.
2. **Interface call, CFlat struct prvalue holding a C++ class field refused** (IFACECOPY review, same on master):
   with `RvRN` above, `struct H { RvRN value = nullptr; }; interface IH { int n(H h); }` implemented by a CFlat
   class, `ih.n(mkH(fp))` (mkH returns an H by value) is refused "cannot pass C++ class 'H' through interface
   method 'IH.n': the argument must have addressable storage ..."; clang (virtual-base twin) accepts. Site:
   CallInterfaceMethod (LLVMBackend_WinRT.cpp ~6576) - the holder call result has no Storage.
