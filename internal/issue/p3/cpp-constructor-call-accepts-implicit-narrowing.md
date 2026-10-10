Bucket: C (ruling needed: scalar conversion table at C++ calls)

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

## 2026-09-28 C1 findings

See `scratch/c1_matrix_r3.md` in the main checkout. Measured on all 160 cells: master constructor/free acceptance differs in 80/160 source/parameter/form cells. The free-function path refuses widening cells: `bool -> float/double` (variable and literal), `char -> float/double` (variable and literal), `short -> float/double` (variable and literal), `u8 -> float/double` (variable and literal), `u32 -> double` (variable and literal), and `int -> double` (variable and literal). Needs a maintainer ruling on the scalar conversion table at C++ call arguments (constructors + free functions) before any fix.

- (C6 review 4, 2026-09-29) Integer narrowing at `new` is accepted on master and after C6: long -> int, int -> char, int -> short, long -> short (probes cflat-fix-c6 scratch/rev4/, kept in scratch/repro_keep/c6). Falls under the no-implicit-narrowing-at-calls ruling.

Related (T59 review 2, 2026-10-06, pre-existing): `pp.P * 3` against a free `operator*(const P&, double)` is
refused ("no overload"); clang++ binds it (int -> double) and returns 13. Same scalar-conversion-table ruling.

## Ruling (maintainer, 2026-10-09)
Scalar conversion table at C++ call arguments, constructor AND free function alike: implicit
WIDENING is accepted everywhere (int / short / bool / unscoped enum -> double, int -> long, ...);
implicit NARROWING is refused (2026-09-04 rule; int -> bool the one exception). Overload
ambiguity stays C++'s: `P4(0)` (int -> long vs pointer) remains ambiguous. Integer narrowing at
`new` (C6 review 4) is refused under the same rule.
