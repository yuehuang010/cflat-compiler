# A C++ exception unwinding through a CFlat frame skips that frame's destructors

Found 2026-09-16 by the C++-interop bug bash round 2 (macOS arm64, Release, master 24016619).

## Summary

CFlat functions are emitted with plain `call` instructions, no personality function and no cleanup
landing pad. When a C++ callee throws, the unwinder walks straight past the CFlat frame: control
reaches the C++ `catch` correctly, but every destructor the CFlat frame owed - here a C++ class
member with a real `~T()` - is never run. The object leaks silently; there is no diagnostic at
compile time and no abort at run time.

This is not the already-ruled "throwing C++ calls are allowed by default" behaviour
(`cpp-noexcept-default` / `--cpp-strict-noexcept`). That ruling is about whether the call is
*permitted*; this is about what happens to the CFlat frame's cleanup when the exception actually
travels through it.

## Repro

`scratch/bb2_ex.h`:

```cpp
#pragma once
namespace bb2ex {
struct Tr { inline static int live = 0; Tr() { ++live; } ~Tr() { --live; } };
inline int thrower(int v) { if (v > 0) throw v; return v; }
typedef int (*Cb)(int);
inline int guarded(Cb f, int x) { try { return f(x); } catch (int e) { return -e; } }
inline int liveCount() { return Tr::live; }
}
```

`scratch/bb2_ex2.cb`:

```cflat
import cpp "bb2_ex.h" cache;

int callee(int x)
{
    bb2ex.Tr t = default;
    return bb2ex.thrower(x);
}

extern int main()
{
    int r = bb2ex.guarded(callee, 5);
    printf("r=%d live=%d\n", r, bb2ex.liveCount());
    if (r != -5) return 101;
    if (bb2ex.liveCount() != 0) return 102;
    return 0;
}
```

    x64/Release/cflat scratch/bb2_ex2.cb -i scratch -o scratch/bb2_ex2.out    # exit 0
    scratch/bb2_ex2.out
    r=-5 live=1
    run=102

`r == -5` proves the unwind reached the C++ catch; `live == 1` is the leaked `Tr`.
`scratch/bb2_ex3.cb` (same, with no destructor-bearing local in the CFlat frame) exits 0, so the
unwind path itself is fine.

## Root cause

`--symbol-dump-ir function:callee` on the same file:

```llvm
define dso_local i32 @"_callee$int$.1$int"(i32 %x) #0 {
entry:
  ...
  %0 = call ptr @_ZN5bb2ex2TrC1Ev(ptr %t)
  %2 = call i32 @_ZN5bb2ex7throwerEi(i32 %1)
  %3 = call ptr @_ZN5bb2ex2TrD1Ev(ptr %t)
  ret i32 %2
}
```

No `personality`, no `invoke`, no `landingpad` - 0 occurrences in the whole dump. The destructor
call is only on the fall-through path.

## Fix direction

When a CFlat function's frame owes any destructor AND it calls a C++ function that is not
`noexcept`, emit the call as an `invoke` with a cleanup landing pad (`__gxx_personality_v0`,
`landingpad ... cleanup`, destructors, `resume`). If that is too costly to do generally, the
cheaper interim is a diagnostic: refuse (or warn once through `LogError`) when a possibly-throwing
C++ call sits in a frame that owes cleanup, so the leak is not silent. The same gap applies to
CFlat `~T()` destructors and to owning `unique`/`new` locals, which would leak memory rather than
just a counter.

Suggested bucket: **p2** (silent resource leak / cleanup skipped; memory-safety adjacent).
