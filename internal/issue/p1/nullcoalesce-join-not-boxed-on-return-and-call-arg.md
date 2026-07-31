# A `??` join into an interface is still unboxed in RETURN and CALL-ARGUMENT position

Filed 2026-07-31 alongside the fix that closed the DECL-INIT and ASSIGNMENT spellings
(`fix/iface-boxing`; see the landed design record in [[interface-issue-queue]]). Those two
spellings now route the join through `UpcastNullCoalesceToInterface` /
`BoxInterfaceJoinArms`. The RETURN and CALL-ARGUMENT paths were never wired to it, and both
still fail.

## Repro 1 - RETURN: module-verifier dump, no source location (the P1 half)

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };

IShape pick(Circle* p, Circle* q) { return p ?? q; }

extern int main()
{
    Circle* a = new Circle(); a.r = 3;
    printf("%d\n", pick(nullptr, a).area());
    return 0;
}
```

Exit 1 on the pre-fix binary, on master, and on the fix branch:

```
Module verification failed:
Function return type does not match operand type of return inst!
  ret ptr %4
 %__iface_fat_ptr = type { ptr, ptr }
```

The `?:` spelling of the same function (`return c > 0 ? p : q;`) is boxed, because the return
path calls `UpcastTernaryPhiToInterface` and a `?:` result IS a phi.

## Repro 2 - CALL ARGUMENT: false rejection (milder - it has a source location)

```cflat
int take(IShape s) { return s.area(); }
...
Circle* z = nullptr;
printf("%d\n", take(z ?? a));
```

```
ib_r2.cb(4,84): no overload of 'take' matches the given arguments.
  Call arguments (1):
    [0] ptr <this>
```

Identical on both binaries. The argument is still a thin `ptr`, so no interface candidate scores.

## Root cause

Same as the closed decl-init half: `??` lowers through a SLOT
(`ParseConditionalExpression`, the `QuestionQuestion()` branch in `MainListener.h`), so its
result is a plain `LoadInst` carrying no `TypeName` and no recoverable arms. The lowering now
ledgers the arms (`LLVMBackend::RegisterNullCoalesceJoin`) and
`UpcastNullCoalesceToInterface` reads them back - but only the decl-init and assignment sites
call it. The return path and the call-argument path still only try the PHI-shaped
`UpcastTernaryPhiToInterface`, which cannot match a load.

## Fix direction

Route both sites through `UpcastPointerJoinToInterface` (the `?:`-then-`??` wrapper) instead of
`UpcastTernaryPhiToInterface`, exactly as the decl-init and assignment sites already do. The
return path additionally needs the `transferArmOwnership` / `armNotOwned` handling it already
applies to `?:`, since a `move`-interface return escapes the frame - do NOT copy the plain
receiver-borrows rule there.

Whatever happens, the RETURN spelling must not reach the module verifier: if the arms cannot be
recovered, emit the same `cannot convert '??' arm to interface '<I>'` diagnostic the decl-init
spelling now emits.

## Related

[[interface-boxing-keyed-on-source-binding]], [[interface-issue-queue]]
