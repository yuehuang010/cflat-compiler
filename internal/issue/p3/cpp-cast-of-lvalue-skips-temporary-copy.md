# C++ interop: assignment to a cast result is not parsed

The T454 fix covers same-type casts of C++ class lvalues in value contexts. One issue remains: C++ accepts assignment to the prvalue temporary created by `(Box)b`, while CFlat rejects the expression before code generation.

## Remaining item

| Repro | CFlat | clang++ -std=c++20 |
|-------|-------|--------------------|
| `(rv.Box)b = x;` | parse error: found `=` but expected `;` | accepted; assigns to the temporary and leaves `b` unchanged |

## Repro

`scratch/repro_keep/box1017/t339_rev/assign_target.cb` and `assign_target.cpp`, with `probe.hpp`.

## Fix direction

Extend the grammar and assignment handling to accept assignment to this cast prvalue and apply the assignment to its temporary. This item was explicitly out of scope for T454 and remains active.
