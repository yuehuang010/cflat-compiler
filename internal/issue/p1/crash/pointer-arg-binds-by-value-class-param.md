# A `T*` argument binds a by-value `T` parameter and reaches the module verifier

Pre-existing, language-wide, and NOT specific to interfaces or to `??`. Found while reviewing
`fix/iface-join-return-boxing` (2026-07-31); confirmed unchanged by that branch and identical on
master, since the repro contains no join at all.

## Repro

```cflat
class Circle { int r = 0; };
int byVal(Circle c) { return 1000 + c.r; }

extern int main()
{
    Circle* a = new Circle(); a->r = 3;
    return byVal(a);            // a Circle*, not a Circle
}
```

```
Module verification failed:
Call parameter type does not match function signature!
  %5 = load ptr, ptr %a, align 8
 %Circle = type { i32 }  %6 = call i32 @_byVal_int_Circle_(ptr %5)
```

Exit 1, **no source location** - the same class of failure the `??`-into-interface P1 had. The
call should either be rejected with a located diagnostic or auto-dereference; silently scoring a
perfect match and lowering a raw pointer into a struct slot is neither.

## Root cause

`TypeAndValue::IsTypeMatch` (`cflat/LLVMBackend.h:624`) returns true on bare `TypeName` equality
and never consults `Pointer`:

```cpp
bool IsTypeMatch(const TypeAndValue& other) const
{
    if (TypeName == other.TypeName)
        return true;
    ...
}
```

So in `ComputeOverloadFunction` an argument `{TypeName="Circle", Pointer=true}` scores a PERFECT
match (result 0) against a parameter `{TypeName="Circle", Pointer=false}`. The sibling
`IsTypePromotion` twenty lines below DOES gate on `Pointer` (`if (Pointer != other.Pointer) return
false;`), which is what makes the omission look accidental rather than deliberate.

The lowering then has no shape check either: the by-value arm of `CreateOverloadedFunctionCall`
passes the value through `Upconvert`, which cannot turn a `ptr` into a struct, and the mismatch
surfaces only at module verification.

## Why this is filed separately

`fix/iface-join-return-boxing` hit this hole from a new direction - its first cut stamped a `??`
join's arm CLASS onto the argument, which handed `IsTypeMatch` exactly the shape above and
produced this dump for `byVal(z ?? a)`. That cut was replaced: the join is now BOXED into a
resolved interface rather than stamped, so nothing in that change reaches this hole any more
(`Test/errors/err_nullcoalesce_iface_arm_unresolved.cb` pins the join spelling as a LOCATED
rejection). The underlying `IsTypeMatch` hole is untouched and predates it.

## Fix direction

Add the `Pointer` gate to `IsTypeMatch`, mirroring `IsTypePromotion`. This is a SCORER change with
a wide blast radius - every overloaded call in the repo scores through it - so it wants the
whole-corpus differential sweep (`Test/`, `example/`, `cflat/core/`, both binaries, compile AND
run) rather than targeted probes, and a check of whether any core library leans on the current
permissiveness. `ElemPointer` / `IsArrayView` deserve the same question in the same pass.

Note `cflat/LLVMBackend.h` is contended - coordinate before editing it.

## Related

[[interface-issue-queue]]
