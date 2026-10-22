# p3: a value and an enum with the same name in ONE scope - misleading "Undefined variable A." (NEEDS RULING)

Pre-existing; found by the T379 review (2026-10-21). Repros: scratch/repro_keep/box1021/t379_rev/param_same_name_enum.cb,
local_same_name_enum.cb, clang oracle same_name_oracle.cpp.

```cflat
int f(int n) { enum n : int { A = 3 }; return (int)n.A; }   // master: "Undefined variable A."
```

Same for a local `int n` declared before the enum in the same block, and inside constructors. clang accepts the C++ equivalent
(`n::A`, exit 35 in the oracle): in C++ the enum is still reachable through the elaborated/qualified name.

T371 (landed) refuses a block enum that SHADOWS an outer value with "'n' names a type, not a value"; this is the same-scope case, where
both names are introduced in one scope.

## Ruling needed

(A) accept like C++: `n.A` resolves to the enum member, bare `n` is the value; or (B) refuse the enum declaration (or the use) with the
T371 text "'n' names a type, not a value". Either way the "Undefined variable A." text is wrong.
