# C++ interop: an identity cast of a C++ class lvalue does not make a temporary copy

Found by the T339 review (box 2026-10-17); the bug was already present on master.

## Summary

In C++, `(Box)b` with `b` an lvalue of class `Box` is a prvalue: the copy constructor builds a temporary, and the temporary dies at the end of
the full expression. In cflat, the cast is treated as `b` itself:

| Repro | cflat (master) | clang++ -std=c++20 |
|-------|----------------|--------------------|
| `rv.take((rv.Box)b);` (overloads `take(const Box&)` / `take(Box&&)`) | `const&` overload chosen, no copy (7/1/0/0/1/1) | `&&` chosen on a fresh copy (7/2/1/0/2/2) |
| `((rv.Box)b).mutate(); v = b.id();` | mutates `b`: v = 99, no copy | mutates the temporary: v = 7, one copy |
| `(rv.Box)b = x;` | parse error "found '=' but expected ';'" | accepted; assigns to the temporary, `b` unchanged |

Columns are value/born/copies/moves/dead/chosen.

## Repro

`scratch/repro_keep/box1017/t339_rev/`: `probe.hpp`, `arg_lv.cb`, `mutate_lv.cb`, `assign_target.cb`, plus `.cpp` oracles.

## Fix direction

When a cast of a class lvalue to its own class type is an argument, a receiver, or otherwise needs a value, materialize a temporary with the
copy constructor (destroyed at the end of the full expression) instead of folding the cast into the source lvalue. T339 handles the prvalue
operand (`(Box)make()`); this issue covers the lvalue operand. Assigning to a cast temporary (the third row) is low value; a maintainer can rule
it out of scope.
