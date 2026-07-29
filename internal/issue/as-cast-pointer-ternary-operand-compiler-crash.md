# 'as' / 'is' on a parenthesized pointer '?:' operand crashes the compiler

Filed 2026-07-28, found during the round-3 review of the stack-value `as` cast fix
(`as-cast-stack-value-to-interface-compiler-crash.md`, now fixed and deleted).

PRE-EXISTING: not introduced by that fix. The operand is `ptr`-typed, so
`ConcreteStructNameFromValue` returns "" exactly as the pre-fix code did, and the
crashing shapes are assignments that never reach the return path the fix touched.

Severity: COMPILER CRASH (SIGSEGV, no diagnostic, ZERO output) reachable from plain
source. No `&`, no stack values, no unusual constructs.

## Repro

```cflat
interface IShape { int area(); };
class Square : IShape { int s = 0; int area() { return s * s; } };

extern int main()
{
    Square* a = new Square(); a.s = 3;
    Square* b = new Square(); b.s = 4;
    int c = 1;
    IShape s = (c > 0 ? a : b) as IShape;
    printf("%d\n", s.area());
    return 0;
}
```

`cflat.exe repro.cb --run` exits 139 having written 0 bytes. The `is` spelling
crashes identically:

```cflat
bool t = (c > 0 ? a : b) is IShape;
```

The plain spelling works and is the control - `IShape s = (c > 0 ? a : b);` compiles
and runs correctly, so this is specific to the `as` / `is` path.

## Notable

- `--check` PASSES clean (`Checked 1 file(s), 0 failed.`), so the fault is at emission,
  not during the listener walk.
- `CompilerManager.h`'s crash handlers do NOT fire - there is no state dump and no
  output at all, which is itself worth understanding.
- With `-o` the compile "succeeds" and the produced exe segfaults instead.

## Root cause

Not diagnosed. Presumed the same fall-through family as
[[as-cast-array-shaped-source-no-diagnostic]]: `GenerateSafeCast` /
`GenerateIsCheck` (`cflat/MainListener.h`) decide "concrete source" from the operand's
type, and a shape they do not recognise falls through to the INTERFACE-source path,
which treats the operand as a fat pointer. Here the operand is a `select` of two
`ptr` values.

## Fix direction

Adopt the positive-routing decision that
[[as-cast-array-shaped-source-no-diagnostic]] already recommends: decide the source
category explicitly (concrete pointer / concrete value / interface fat pointer) and
`LogError` on anything that matches none, instead of falling through to the interface
path by default. That closes this shape, the array-shaped shape, and any future one
in the same family with a single change.

Per CLAUDE.md, a crash reachable from plain source must become a proper `LogError`
once the root cause is known.

## Related

[[as-cast-array-shaped-source-no-diagnostic]] - sibling fall-through, same family.
[[as-boxing-skips-ownership-transfer]] - the structural argument about boxing
bookkeeping being spread across four sites.
