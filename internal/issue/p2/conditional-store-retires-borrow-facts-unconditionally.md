# A conditional `=` retires borrow/bond facts unconditionally

P2, PRE-EXISTING (not introduced by `fix/coalesce-tail`; measured identical on `152728c` and on
that branch). Filed 2026-08-06 by `fix/coalesce-tail` while building the per-subsystem oracle for
routing `??=` through the shared store tail.

## What

`ParseAssignmentExpression`'s store tail RETIRES three declaration-time facts on a plain `=`:

- `ClearVariableBond` - the LHS is no longer bonded to anything.
- `SetVariableBorrowsOwnedString(name, false)` - it no longer borrows an owned string field.
- `SetVariableBorrowsOwnedElement(name, false, ...)` - it no longer borrows a container-owned
  element.

All three are FLOW-INSENSITIVE: they are recorded when the walk reaches the `=`, with no regard for
whether that `=` is reachable at run time. That is correct for a store that always happens and
unsound for one inside a branch, because the fact each retires is what stands between the binding
and a `delete` of memory somebody else owns.

## Repro (measured on `152728c`, unchanged on `fix/coalesce-tail`)

```cflat
import "list.cb";
int dtors = 0;
struct R { int v = default; ~R() { dtors = dtors + 1; } };
extern int main()
{
    {
        list<unique R*> l = default;
        l.add(new R());
        R* g = l.get(0);                       // g borrows an element the list owns
        if (g == nullptr) { g = new R(); }     // NEVER taken - g is not null
        delete g;                              // accepted: the taint was cleared anyway
        printf("mid dtors=%d\n", dtors);
    }
    printf("end dtors=%d\n", dtors);
    return 0;
}
```

`mid dtors=1`, then the list's destructor frees the same object: rc 133 (SIGTRAP, double free).
Deleting the `if` line makes the same `delete g` a hard error
(`cannot delete 'g' - it borrows an element that its container owns`), which is what shows the
unreachable store is the whole cause.

## Root cause

The three calls above run once per syntactic `=`, keyed on the destination NAME, and there is no
per-path state. The compile-time ledger has one slot per binding and no notion of "cleared on this
path only".

## Why it is filed rather than fixed

`fix/coalesce-tail` needed an answer for `??=`, which is `if (x == null) x = rhs;` and therefore hits
exactly this shape. It took the JOIN there instead (keep the fact unless the new RHS also carries
it), which is conservative, matches what master already does for `??=`, and needed no new state. The
general `=`-inside-a-branch case cannot be fixed the same way: `if (c) { g = new R(); } delete g;`
with `c` true is CORRECT code and joining would false-reject it. A sound fix needs a real
flow-sensitive fact (a per-path or MAY/MUST lattice like the one `nulldf` already runs), which is a
feature, not a clause.

## Oracle pairs measured

| spelling | element taint after | `delete g` | run |
|----------|--------------------|------------|-----|
| `g = new R();` (unconditional) | cleared | accepted | correct, `dtors` 1 then 2 |
| `if (g == nullptr) { g = new R(); }` (never taken) | cleared | accepted | rc 133, double free |
| `g ??= new R();` (never taken) | KEPT (join, `fix/coalesce-tail`) | rejected | n/a |

The bond twin is the same shape: `int* view = Borrow(&a); if (c) { view = &b; } a = 6;` clears the
bond on the walk and lets `a` be reassigned while `view` may still point at it.

## Fix direction

Make the three retirements MAY/MUST-aware rather than walk-order, reusing the block-keyed machinery
`nulldf` already has, or record the retirement against the block it happened in and resolve at the
consuming site. Do NOT convert them to joins - that false-rejects the always-taken branch, which is
the common spelling.

## Related

`coalesce-assign-skips-store-bookkeeping` (fixed and deleted by `fix/coalesce-tail`) - its landed
record in `interface-issue-queue.md` carries the full per-subsystem oracle table this was split out
of.
