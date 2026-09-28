# A C++ class with both `A()` and `A(int = 5)` default-constructs through `A()` instead of refusing

C++ rejects `A a;` / `A()` as ambiguous when a class declares both a nullary constructor and a
constructor whose parameters are all defaulted. CFlat `A a = default;` silently picks `A()`.
Pre-existing (master and fix/defarg-ctor both return 19 on the probe). Found 2026-09-27 reviewing
fix/defarg-ctor (cell `AmbM`, scratch/dac_matrix.md). Adding the refusal is full mode (accept-set).

## Repro

```cpp
// am.h
#pragma once
namespace am { struct AmbM { int v; AmbM() : v(19) {} AmbM(int x = 5) : v(x) {} }; }
```

```cflat
import cpp "am.h";
extern int main() { am.AmbM a = default; return a.v; }   // 19; want: refused as ambiguous
```

## Fix direction

When more than one constructor is callable with zero arguments, let clang decide (the generated
`new (p) T();` wrapper already reports "call to constructor of ... is ambiguous") instead of
FindCxxDefaultCtor preferring the nullary one.
