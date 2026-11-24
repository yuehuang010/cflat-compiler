# C++ class converting ctor: global initializers refused; interface call copies once more than clang

CONVREST (2026-11-19) fixed the mixed-pointee `?:` (composite pointer type, derived arm adjusted
before the PHI) and the `T**` source ([conv.qual]) of cpp-class-pointer-converting-ctor-remaining-sources.
Two leftovers remain. Measured 2026-11-19 on the CONVREST exe (cold = warm).

Repro header (rv.h):

```cpp
static int rv_ctor = 0, rv_dtor = 0;
struct RvFoo { int value = 3; };
struct RvR { int value; RvR(const RvFoo* p) : value(p ? p->value : 0) {} };
struct RvRN { int value; RvRN(const RvFoo* p) : value(p ? p->value : 0) { rv_ctor++; }
              RvRN(const RvRN& o) : value(o.value) { rv_ctor++; } ~RvRN() { rv_dtor++; } };
struct RvI { int value; RvI(int i) : value(i) {} };
struct RvPP { int value; RvPP(const RvFoo* const* p) : value(9) {} };
inline int rv_counts() { return rv_ctor * 10 + rv_dtor; }
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
2. **Interface call extra copy:** `interface IRv { int n(RvRN r); }` implemented by a CFlat class;
   `i.n(fp)` and `i.n(RvRN(fp))` give ctor 2 / dtor 2 (clang 1 / 1, balanced, no leak). A direct
   CFlat callee and a named local match clang. Not traced; the interface-call argument path
   (LLVMBackend_WinRT.cpp, MaterializeImplicitCxxClassArgument for an interface method) is where
   the pointer source is converted, and the spelled `RvRN(fp)` source shows the same extra copy,
   so the copy is in the interface dispatch of a by-value C++ class, not in the conversion.

Found by: CONVPTR round-2/3 review, 2026-11-19; re-measured by CONVREST.
