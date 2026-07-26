# Direct assignment of a T** / T[] binding to an interface slot silently type-confuses

Filed 2026-07-26, found during review of the ternary borrow-arm fix (branch
fix/ternary-iface-borrow-arm-module-verify). Pre-existing on master; the ternary spelling of
the same hole was closed by that fix (its resolver rejects ElemPointer / IsArrayView / etc.
and its call sites emit "cannot convert '?:' arm to interface ..."), but the DIRECT
assignment path never goes through UpcastTernaryPhiToInterface and still mis-accepts.

## Repro

```cflat
interface IShapeMove { int area(); };
class SqMove : IShapeMove { int s = 3; SqMove() { } int area() { return s * s; } };

extern int main()
{
    unique SqMove* owner = new SqMove();
    SqMove** pp = &owner;
    IShapeMove k = pp;        // accepted; boxes SqMove vtable over a SqMove** slot
    return k.area();          // reads garbage (returned 0 in testing)
}
```

Also reproduces with an array view: `SqMove[] v = new SqMove[4]; IShapeMove k = v;`
(returned 9 in testing - reads the first element's storage as if it were the object, which
happens to line up, but the shape is still wrong: no per-element identity, wrong ownership).

## Root cause

The ordinary class->interface upcast on declaration-init/assignment trusts the binding's
declared TypeName ("SqMove") without checking the pointer SHAPE (ElemPointer, IsArrayView,
ConstArraySize, IsSimd). A T** or T[] loads to a bare ptr, StructImplementsInterface("SqMove",
...) succeeds, and the value is boxed with the class vtable pointing at a pointer/view slot.

## Fix direction

Apply the same shape guard the ternary fix added (see the resolver's pick lambda in
cflat/LLVMBackend.h, branch fix/ternary-iface-borrow-arm-module-verify: reject !Pointer ||
ElemPointer || IsArrayView || IsSimd || IsInterface || IsInterfacePointer || ConstArraySize
!= 0) at the direct upcast site(s) in cflat/MainListener.h, and emit a LogErrorContext
("cannot convert 'SqMove**' to interface 'IShapeMove'" family). Grep for where decl-init and
'=' box a thin class pointer into a fat interface value outside the ternary path. Add
expect_error legs next to the ternary-arm ones in Test/errors/err_move.cb (or a more natural
interface error file) for both the T** and T[] spellings.
