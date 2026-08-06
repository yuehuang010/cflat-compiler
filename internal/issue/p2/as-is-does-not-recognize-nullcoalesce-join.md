# `as` / `is` does not recognize a '??' join at all

Filed 2026-08-05 from the coverage matrix of the chained-join boxing fix. NOT a chaining defect -
it reproduces at chain length 1, and is measured identical on `4c06cce` and on the fix binary.

## Repro - SINGLE '??', no chain

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };

extern int main()
{
    Circle* a = new Circle(); a->r = 3;
    Circle* z = nullptr;
    IShape j = (z ?? a) as IShape;
    return j.area();
}
```

```
s_as_ncsingle.cb(5,13): 'as' requires an interface value or a class instance on the left of
'IShape'; this expression is neither
```

The `is` spelling gives the matching message at its own column, and the chained spelling
(`z ?? y ?? a`) gives the same message - the chain is irrelevant, the join spelling is the issue.

## Contrast that localizes it

The '?:' spelling of the SAME construct works: `(ib(true) ? a : b) as IShape` compiles and
dispatches. So this is specific to '??'.

## Root cause

The cast-source classifier (`ClassifyCastSource` in `cflat/MainListener.h`, in its
`CastSourceKind::TernaryPointerJoin` arm) recognizes a pointer JOIN only when the
value is a `PHINode`:

```cpp
// A '?:' join of pointers carries no elemType; each arm has its own concrete class.
if (valueType->isPointerTy())
    if (auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
        if (phi->getNumIncomingValues() > 0) return CastSourceKind::TernaryPointerJoin;
```

A '??' joins through a SLOT, so its result is a `LoadInst`, never a `PHINode` - it falls through to
`CastSourceKind::Unknown` and the located "neither" diagnostic. The arms are recoverable: the
lowering ledgers them and `FindNullCoalesceJoin` reads them back, which is exactly how the boxing
path handles the same value.

## Fix direction

Extend the classifier to accept a ledgered '??' load as a pointer join, then let the existing
per-arm cast machinery run - `CollectPointerJoinArms` (`cflat/MainListener.h`) already returns the
arms of either spelling in one call, so the downstream per-arm code needs no new shape. Note the
interaction: `ResolveTernaryArmClasses` does a `llvm::cast<llvm::PHINode>` on its input, so it must
be moved onto the same collector in the same change or it will assert on the newly-admitted load.

Verify the accept-set first - this widens what `as`/`is` ACCEPTS, and the neighbouring
[[nested-join-arm-unresolved-in-is-as-and-mixed-ternary]] shows the concrete-class predicate is
still not join-aware, so a '??' admitted here must not silently answer off one arm.

Workaround: bind the join to a concrete `T*` local first, then cast that.

## Related

[[nested-join-arm-unresolved-in-is-as-and-mixed-ternary]], [[interface-issue-queue]]
