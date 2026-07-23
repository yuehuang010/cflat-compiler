# `unique <interface>` rejects explicit `move` between locals (over-strict predicate)

Opened 2026-07-22 during the assignment-semantics probe for
`internal/plan/ownership-transparent-assignment.md`.

## Summary

A `unique IThing` local cannot be transferred to another `unique IThing` variable at all:
`unique IThing b = move a;` and `b = move a;` are both rejected with
"cannot initialize/assign a borrowed value to unique interface ... - the source still owns
it". Only `new ...`, a move-returning CALL, or `nullptr` satisfy the check. This is
inconsistent with thin `unique R*`, where `move a` works for init. Workaround exists:
routing through `move IThing passthrough(move IThing x) { return move x; }` transfers fine,
proving the transfer itself is sound - only the local-to-local predicate is wrong.

## Repro (compile error; expected: transfer)

```cflat
interface IThing { int get(); };
class Thing : IThing { int v = 0; int get() { return v; } };
extern int main()
{
    unique IThing a = new Thing();
    unique IThing b = move a;   // ERROR: cannot initialize unique ... from a borrowed value
    return 0;
}
```

## Root cause

Two structurally different ownership predicates (mapped in the plan's code survey):
- Thin `unique T*` init (MainListener.h 8487-8496) gates OUT on syntactic `move`
  (`!srcIsMove` guards the whole reject), so `move a` always passes.
- Unique-interface assign (MainListener.h 9517-9526, flags captured at 9291-9293) requires
  the source be PROVABLY owned via `lastOwningResult || lastCallReturnsOwned ||
  rightNV.TypeAndValue.IsMove`; for a named stack local holding a heap-boxed interface value
  these flags do not line up, and the stack-boxing rejection downstream (comment 9504-9510)
  catches what the init path would admit. Net effect: no local-to-local transfer form exists.

## Fix direction

Unify the two predicates on one "source is a consumable owner" test (the same keying the
move-sink machinery uses in ApplyMoveParamTransfer): an explicit `move <named unique local>`
must transfer for interface values exactly as for thin uniques - consume + null the source,
adopt in the destination (freeing any old destination value via DeleteInterfaceValue, which
the reassign path already does). Part 3 of
`internal/plan/ownership-transparent-assignment.md`.
