# C++ constructor call arguments accept implicit narrowing (function calls refuse it)

Full mode (adds a rejection). `T(2.5)` into a C++ constructor taking `int` compiles and truncates;
the same argument into a C++ free function taking `int` is refused ("no overload ... matches").
The 2026-09-04 ruling (no implicit narrowing at call arguments; int -> bool the one exception) and
"extern C calls follow CFlat rules" apply to constructor calls too. Found 2026-09-26 by the
converting-constructor fix (plan phases 1 + 3): `cv.Cnt c = 2.5;` follows the spelled `cv.Cnt(2.5)`
form ("as if the user wrote `T(source)`"), so it narrows too; fixing the constructor call fixes both.

## Repro (standalone, ~1 s)

```cpp
// nar.h
#pragma once
namespace nar { struct Cnt { int v = 0; Cnt(int x) : v(x) {} }; inline int take(int x) { return x; } }
```

```cflat
import cpp "nar.h";
extern int main()
{
    nar.Cnt c = nar.Cnt(2.5);   // master 613d586c: compiles, c.v == 2 (want: refused)
    int t = nar.take(2.5);      // refused: no overload of 'nar.take' matches
    return c.v + t;
}
```

## Fix direction

Apply the CFlat call-argument conversion rules (the ones `nar.take` uses) to C++ constructor
overload selection, including template constructors after deduction. Accept-set first: every
constructor call in `Test/` and `test_libs/` that passes an exact or widening argument must keep
compiling. Native CFlat constructors: check whether they have the same hole and cover both.
