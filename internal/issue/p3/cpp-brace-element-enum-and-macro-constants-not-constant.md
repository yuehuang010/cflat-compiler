# Imported C++ enum constants and macro constants are not constants in a brace list

Found by T63 review 2 (pre-existing; master's brace-argument path refuses the same way).

## Repro (scratch/repro_keep/t63_rev/r2/)
```cflat
std.vector<short> a{pr.Red, 2};     // cflat: refused as narrowing (non-constant); clang accepts
std.vector<short> b{PR_SMALL, 2};   // #define PR_SMALL 5 - same
```
They load from mutable globals in CFlat, so T63's DirectBraceConstantText never sees a constant and
clang's narrowing check treats them as runtime values.

## Fix direction
Fold imported unscoped-enum constants and integer macro constants to their literal text when
emitting a brace element (they are compile-time constants in C++).
