# A '?:' join of two interface values fails overload resolution as a by-value interface argument

Filed 2026-07-26, found during round-2 review of the fat-vs-thin ternary fix
(branch fix/ternary-move-arm-fat-vs-thin). Confirmed pre-existing on master
(binary at 5f3c472): identical output on master and the fix branch. No ternary-move,
no inheritance, no thin arm needed - just a '?:' of two interface locals passed by value.

## Repro

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 4; Circle() { } int area() { return r * r; } };
bool identityBool(bool b) { return b; }
int use(IShape s) { return s.area(); }

extern int main()
{
    IShape a = new Circle();
    IShape b = new Circle();
    printf("arg=%d\n", use(identityBool(true) ? a : b));
    return 0;
}
```

Output:

```
repro.cb(10,23): no overload of 'use' matches the given arguments.
  Call arguments (1):
    [0] __iface_fat_ptr <unnamed>
  Candidates (1):
    _use_int_IShape_(IShape s)
  Argument mismatch detail (single resolved candidate: _use_int_IShape_):
    [0] arg=__iface_fat_ptr  param=IShape
```

## Root cause

The '?:' join of two interface values produces a phi whose TypeAndValue carries no
IsInterface flag / interface TypeName - overload resolution sees the raw LLVM struct
name "__iface_fat_ptr" instead of the interface name, so the by-value IShape parameter
never matches.

## Fix direction

When both arms of a ternary are interface values of the same interface (or after the
fat-vs-thin harmonizer resolves them to the receiver interface), stamp the joined
TypeAndValue with IsInterface + the interface TypeName so downstream overload
resolution treats it as an IShape value. Likely near UpcastTernaryPhiToInterface /
the ternary join TypeAndValue construction in cflat/MainListener.h. Add a regression
leg to Test/test_move.cb (call taking the join by value) once fixed.
