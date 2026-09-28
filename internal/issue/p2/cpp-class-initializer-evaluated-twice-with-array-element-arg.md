# C++ by-value class initializer is EMITTED TWICE when a nested ctor argument reads an array element

`T v = f(C(arr[i]));` - where `f` returns a C++ class by value and a nested C++ constructor
argument reads an array element - calls `f` (and `C`) twice. The first result goes to a discarded
`cxx.rettemp` and is destroyed; the second is copied into `v`. Side effects run twice. No diagnostic.

Found by the libtorch ladder (2026-09-26): t23 `torch.randint(0, 5, c10.IntArrayRef(&tsz[0], 1))`
draws twice from the seeded RNG, so `target` is the second draw and `ce=3.038822` instead of the
reference `2.463950` (Python torch, seed 7, target [0, 2, 3, 2]). `torch.ones(c10.IntArrayRef(&d[0], 2))`
in t17/r17c is also invoked twice (harmless there, but wasted work).

## Repro

`scratch/refbug/de.h`, `de2.cb`, `de3.cb`:

```cpp
#pragma once
namespace de {
inline int calls = 0;
struct Sz { long n; Sz(long x) : n(x) {} };
struct Obj { long v; Obj(long x) : v(x) {} Obj(const Obj& o) : v(o.v) {} ~Obj() {} };
inline Obj make(Sz s) { ++calls; return Obj(s.n); }
inline int count() { return calls; }
}
```

```cflat
import cpp "de.h";
extern int main()
{
    long[1] n = {4};
    de.Obj a = de.make(de.Sz(n[0]));
    return de.count();   // 2, want 1
}
```

Evaluated ONCE (correct): `de.make(de.Sz(k))` with `k` a local, `de.Sz(4)`, `de.Sz(id(4))`,
and default-argument wrappers with those arguments. The array-element read in the nested argument
is the trigger (`&d[0]` in the libtorch spelling as well).

## Evidence (de3.cb, --out-lli)

```
%1 = call ptr @_ZN2de2SzC1El(ptr %0, i64 4)
call void @_ZN2de4makeENS_2SzE(ptr sret %cxx.rettemp, ...)      ; first, discarded
%3 = invoke ptr @_ZN2de2SzC1El(ptr %2, i64 4)
invoke void @_ZN2de4makeENS_2SzE(ptr sret %cxx.rettemp2, ...)   ; second
invoke ptr @_ZN2de3ObjC1ERKS0_(ptr %a, ptr %cxx.rettemp2)       ; copy into a
... ObjD1(%cxx.rettemp), ObjD1(%cxx.rettemp2)
```

The first copy is emitted before the invoke context exists (plain `call`), which points at a
speculative/probe emission of the initializer expression (type or provenance probing) whose
instructions are not rolled back, followed by the real emission.

## Fix direction

Find the path that evaluates the initializer expression a second time when an argument contains
an array-element access (likely a provenance / frame-rooted probe over the argument tree) and make
it analysis-only, or reuse the first result. Regression: the counter shape above in
`Test/test_cpp_interop.cb` against an in-repo header (count must be exactly 1), plus the
default-argument-wrapper variant `make_dflt(1, de.Sz(n[0]))`.

## Notes for the fixing agent

- The `de.h` / main above are complete and self-contained; `count()` must be 1. Compiles in ~1 s.
  Variants and their counts on master (all should be 1):
  `de.make(de.Sz(n[0]))` = 2, `make_dflt(1, de.Sz(k))` = 1, `make_dflt(1, de.Sz(id(4)))` = 1,
  `de.make(de.Sz(k))` = 1, `make_dflt(1, de.Sz(4))` = 1, `make_dflt(1, de.Sz(n[0]))` = 2
  (with `inline Obj make_dflt(long a, Sz s, int opt = 3) { ++calls; return Obj(a + s.n + opt); }`).
- Not yet checked (do this first, it narrows the search): a plain CFlat struct instead of the C++
  `Sz`; a CFlat function instead of `de.make`; an assignment `a = de.make(de.Sz(n[0]));` instead
  of an initializer; `n[0]` replaced by `*ptr` or `s.field`. The first copy is emitted before any
  invoke/landing pad exists, which suggests an early probe emission in the declaration path.
- Use a debugger, not print statements: build Debug (`./cmake_build.sh debug`) and break on the
  emission of the call (e.g. `b llvm::IRBuilderBase::CreateCall` conditioned on callee name, or on
  the CFlat call-emission entry) to get both stacks; the first stack names the probe.
- libtorch confirmation (local-only, skip if `scratch/` absent):
  `bash scratch/ladder/torch/run_all.sh t23` -> must print `ce=2.463950` (reference from Python
  torch 2.14, seed 7: `/opt/homebrew/opt/pytorch/libexec/bin/python3`, target [0, 2, 3, 2]).
  Master prints `ce=3.038822` because `randint` draws twice.
