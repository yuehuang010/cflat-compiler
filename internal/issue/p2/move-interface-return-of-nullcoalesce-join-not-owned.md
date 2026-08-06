# A `move <interface>` RETURN of a '??' join is rejected as not owned

Filed 2026-08-05 while fixing [[chained-nullcoalesce-not-boxed-into-interface]]. Found by that
work's coverage matrix. NOT a chaining defect - it reproduces at chain length 1.

## Repro - single '??', no chain

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };

move IShape mk(int k)
{
    Circle* a = new Circle(); a->r = 3;
    Circle* z = nullptr;
    return z ?? a;
}
extern int main() { IShape j = mk(1); return j.area(); }
```

```
cc_move_ret_single.cb(5,74): function declares 'move' return type but returned expression is not
owned - value must come from 'new', a move parameter, or another move-returning function
```

The chained spelling (`return z ?? y ?? a;`) gives the same diagnostic at its own column. Both
measured identical on `4c06cce` and on the chain-fix binary.

## Note on what already works

`Test/test_move.cb` has a passing `move`-interface RETURN leg for a '??' join
(`iface_join_move_return_*`, the OWNERSHIP PIN block). That leg returns a join of two `move`
PARAMETERS. This repro returns a join of a LOCAL fed by `new`, which is the shape the whole-
expression owned-return check cannot prove: the join is a load out of the coalesce SLOT, and
`IsOwningValue` answers only a load off a NamedVariable, so the slot load answers false. The
per-arm ownership machinery (`transferArmOwnership`) exists and would answer correctly - it is
never consulted, because the whole-expression check rejects first.

## Note: the nested form's REMEDY was corrected separately

A neighbouring wrong-remedy bug in the same area was fixed by `fix/chain-coalesce` and is NOT part
of this issue. When a NESTED join's inner arm was provably non-owning, the outer boxing site
discarded the inner `armNotOwned` verdict and reported "bind the arm to a local variable of the
class type first" - a remedy that does not help at a `move` return - while the length-1 form
correctly reported the ownership diagnostic. Measured with one `unique` arm and one borrowed
parameter arm (so the whole-expression check passes and the per-arm check is actually reached):

```cflat
move IShape pick(Circle* borrowed)
{ unique Circle* p = new Circle(); p->r = 2; unique Circle* q = new Circle(); q->r = 4;
  return p ?? q ?? borrowed; }
```

Before: `cannot convert '??' arm to interface 'IShape': the arm's concrete class cannot be
determined; bind the arm to a local variable of the class type first`. After: the same "returned
expression is not owned" message the length-1 spelling gives. Both are rejections either way -
only the wording changed.

## Fix direction

Move the owned-return decision for a JOIN operand to the per-arm walk that boxing already does,
the way `BoxInterfaceJoinArms`' `armNotOwned` path does for the arms it can see. Polarity matters:
this is currently a FALSE REJECTION of a program that should compile, so the fix must widen the
accept side, and the per-arm check must keep rejecting a join with a genuinely borrowed arm (that
is what stops the caller becoming a second owner). Build the accept-set first - see
`internal/fix-issue-lessons.md` "On guard polarity".

Workaround: bind the join to a concrete `T*` local and return that.

## Related

[[interface-issue-queue]]
