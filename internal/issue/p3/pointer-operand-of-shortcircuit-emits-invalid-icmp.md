# A pointer operand of `&&` / `||` emits an invalid `icmp`

Filed 2026-08-10 during the `fix/coalarm` review, found while probing the short-circuit RHS
flush. PRE-EXISTING and unrelated to that fix: identical on `0535f48` and on `fix/coalarm`.
P3: the module fails verification, so nothing is silently miscompiled.

## What

A raw pointer used directly as a `&&` / `||` operand is coerced to bool with an `icmp` whose
right-hand side is an `i0` zero instead of a null pointer, so the module does not verify:

```
Module verification failed:
Both operands to ICmp instruction are not of the same type!
  %tobool1 = icmp ne ptr %7, i0 0
```

The same value used as an `if` condition, or compared explicitly with `!= nullptr`, is fine -
only the short-circuit operand coercion is wrong.

## Repro (`scratch/rev_f1.cb`, `rev_f2.cb`, `rev_f3.cb`)

```cflat
int* getp() { return nullptr; }
extern int main(){ int x = 1; bool r = (x > 0) && getp(); return 0; }  // fails
extern int main(){ int x = 1; bool r = (x > 0) || getp(); return 0; }  // fails
extern int main(){ int x = 1; bool r = getp() && (x > 0); return 0; }  // fails, LHS too
```

Both operand positions are affected, and both operators.

## Fix direction

`ParseLogicalAndExpression` / `ParseLogicalOrExpression` (`cflat/MainListener_Expressions.cpp`)
reduce each operand to a bool before `CreateOperation`. The zero they compare against is built
from the wrong type for a pointer operand; use a null pointer constant of the operand's own type,
the way the `if` condition path (`CreateConditionJump`) already does.
