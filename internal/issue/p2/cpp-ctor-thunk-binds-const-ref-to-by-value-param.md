# C++ ctor thunk takes a scalar argument BY VALUE, so a `const T&` ctor param dangles

An implicit conversion of a scalar to a C++ class at a C++ call argument goes through the
generated `__cflat_ctor_<hash>` thunk (`RequestCxxVariadicConstructor`,
`cflat/LLVMBackend_CInterop.cpp` ~9040-9150). The thunk spells a scalar parameter by value
(`long p1`) and constructs the target from it. A constructor that keeps the address of a
`const T&` parameter - every view type, e.g. `c10::ArrayRef(const T& OneElt)` - then points at
the thunk's own parameter (and, on the `copyInit` path, a lambda local), which is dead when the
thunk returns. Silent wrong value / garbage.

Found by the libtorch ladder (2026-09-26): t17 `o.view(4)` aborts with
`shape '[6130608168]' is invalid for input of size 4` (a stack address).
`o.view(c10.IntArrayRef(&four, 1))` and explicit `c10.IntArrayRef(four)` are fine.

## Repro

In-repo, no libtorch (full program under "Notes for the fixing agent"; local copies
`scratch/refbug/selfcheck/th.h` / `th.cb`). The thunk path is taken when the class
itself has a constructor template (`CxxConstructorNeedsClangResolution`); `clobber()` makes the
dangling read visible:

```cpp
template <class T> struct HArr {
    const T* Data; size_t Length;
    constexpr HArr(const T& one) : Data(&one), Length(1) {}
    constexpr HArr(const T* d, size_t c) : Data(d), Length(c) {}
};
template <class T> struct Arr : HArr<T> {
    using HArr<T>::HArr;
    template <size_t N> constexpr Arr(const T (&a)[N]) : HArr<T>(a, N) {}
    T first() const { return this->Data[0]; }
};
__attribute__((noinline)) inline void clobber() { volatile long junk[64]; for (int i = 0; i < 64; ++i) junk[i] = -7; }
struct Ten { Ten(int) {} long only(Arr<long> a) const { clobber(); return a.first(); } };
```

`t.only(4)` returns garbage (`-6846233876700856149`); want 4. On libtorch `o.view(four)` (a named
local) fails too; in-repo the named-local case binds without the thunk and passes.

## Evidence (libtorch r17c, --out-lli)

```
invoke void @__cflat_ctor_f7ac66a02661c218(ptr %0, i64 4)
define internal void @__cflat_ctor_f7ac66a02661c218(ptr %0, i64 %1)   ; by value
  ... lambda: %3 = alloca i64; store; call ArrayRef<long>::ArrayRef(inherited)(ptr %2, ptr %3)
      lifetime.end(%3); return the ArrayRef   -> Data = &dead slot
```

## Fix direction

When the selected/possible target constructor parameter is a reference, spell the thunk parameter
as `const T &` (or `T &&` for an rvalue) and pass the CFlat caller's materialized temporary, which
lives to the end of the caller's full-expression - C++'s own lifetime for `f(4)` into
`ArrayRef(const long&)`. Keep by-value for genuine by-value parameters. The `copyInit` lambda must
return from the reference, not from a copy. Regression: the `th.h` shape (literal and named local)
in `cpp_interop_*.h` + `Test/test_cpp_interop.cb`, with a stack clobber in the callee so the test
cannot pass by luck.

## Notes for the fixing agent

- Complete self-contained repro (verified 2026-09-26, prints `a=268291553281704078 (want 4)`):
  wrap the header above in `#pragma once`, `#include <cstddef>`, `namespace th { ... }`, and call:

  ```cflat
  import cpp "th.h";
  extern int printf(const char* fmt, ...);
  extern int main()
  {
      th.Ten t = th.Ten(0);
      long a = t.only(4);
      printf("a=%ld (want 4)\n", a);
      return a == 4 ? 0 : 1;
  }
  ```

- The trigger is the class's OWN constructor template (`template <size_t N> Arr(const T (&)[N])`):
  it makes `CxxConstructorNeedsClangResolution` true and routes the conversion through the thunk.
  Delete that line and the call binds directly, no thunk, correct result - verified. So a test
  without the ctor template covers nothing here.
- The `clobber()` in the callee is required; without it the dead slot still holds 4 and the test
  passes by luck. Keep it in the regression fixture.
- Always add `#pragma once` to a new fixture header: the brace/ctor wrapper requests re-include
  the header, and a guardless header fails with a misleading `redefinition of '...'` from clang.
- Inspect the thunk with `--out-lli` and search `define internal void @__cflat_ctor_`: correct
  after the fix = the thunk's parameter is a `ptr` (reference) for a reference target parameter,
  and the caller passes the address of a caller-frame temporary.
- libtorch confirmation (local-only, skip if `scratch/` absent): `bash scratch/ladder/torch/run_all.sh t17`
  -> run_rc=0, output contains `sel=1 zsum=12.000000`. t17 saves to the RELATIVE path
  `scratch/ladder/torch/out2`, so run it from the repo root (run_all.sh does).
