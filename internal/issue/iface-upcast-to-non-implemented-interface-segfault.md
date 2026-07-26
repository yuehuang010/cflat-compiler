# Upcasting a class to an interface it does not implement segfaults instead of erroring

Filed 2026-07-26, found during round-2 review of the fat-vs-thin ternary fix
(branch fix/ternary-move-arm-fat-vs-thin). Pre-existing on master: the plain
non-ternary spelling `unique IOther k = move g;` (where g's class does not implement
IOther) segfaults on the master binary too.

## Repro (non-ternary, master)

```cflat
interface IShape { int area(); };
interface IOther { int poke(); };
class Circle : IShape { int r = 4; Circle() { } int area() { return r * r; } };

extern int main()
{
    unique IShape g = new Circle();
    unique IOther k = move g;    // Circle does not implement IOther
    return k.poke();             // exit 139 - dispatch through null vtable
}
```

The ternary spelling (`unique IOther k = c ? move g : new Circle();`) also segfaults;
before the fat-vs-thin fix, master rejected that particular spelling with
"incompatible types", so the ternary path regressed from error to crash there, but
the underlying missing static check is shared with the direct path.

## Root cause

RebuildInterfaceFatValue (cflat/LLVMBackend.h, ~9645) documents "No match yields a
zeroed fat pointer" - when the runtime typedesc walk finds no vtable for the target
interface, it silently produces a null-vtable fat value and the later method call
dispatches through it. On the ternary path, BoxTernaryThinArmToInterface
(cflat/MainListener.h, ~10935) checks StructImplementsInterface against the fat
arm's interface, never the receiver's declared interface.

## Fix direction

The receiver's interface is statically known at the decl-init/assignment site. Emit a
LogErrorContext ("class 'Circle' does not implement interface 'IOther'" family) when
the source's class/interface set provably lacks the receiver interface - at the direct
upcast site and in BoxTernaryThinArmToInterface (check the RECEIVER interface, not the
fat arm's). As a backstop, RebuildInterfaceFatValue's no-match case should not yield a
silently-null vtable on paths reachable without a prior static check. Add expect_error
legs to Test/errors/err_move.cb for both the direct and ternary spellings.
