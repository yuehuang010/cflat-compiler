# `as` cast of a fixed-ARRAY stack source to an interface: bad code, no diagnostic

Filed 2026-07-28 by an adversarial review of the stack-value `as` fix.
PRE-EXISTING. It is the closest adjacent shape to the crash that fix closed, and it is
deliberately still open.

Severity: accepted source, clean compile (even under the assert-enabled Debug build), and
the resulting exe segfaults. No compiler crash.

## Repro

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };

extern int main()
{
    Circle[3] arr;
    arr[0].r = 2;
    IShape s = arr as IShape;
    printf("a=%d\n", s.area());
    return 0;
}
```

Compiles with no diagnostic; the exe exits 139 (SIGSEGV).

## Root cause

`ConcreteStructNameFromValue` (`MainListener.h:11995`) - the helper that routes a class VALUE
operand into `GenerateSafeCast`'s statically-resolved concrete branch - matches only a named
`llvm::StructType`. A fixed array local loads as `[3 x %Circle]`, an `ArrayType`, so the
helper returns "" and the operand falls through to the INTERFACE-source path, which reads the
array aggregate as if it were an `__iface_fat_ptr`.

This is the same fall-through that made a plain class value crash the compiler. It does not
crash here only because `extractvalue` index 1 happens to be in range for a 3-element array,
so a `%Circle` element is misused as the data pointer instead of asserting.

## Fix direction

An array is not a single object and has no single data slot, so this is a rejection, not a
missing boxing case: an array-shaped operand must be diagnosed the way a pointer-shaped one is
(see [[as-boxing-skips-pointer-shape-rejection]]). The check belongs wherever the boxing
decision ends up living - see the consolidation recommendation in
[[as-boxing-skips-ownership-transfer]] rather than adding a fourth array special-case.

Beware of narrowing this to "reject ArrayType": the fall-through is the bug, so the safe shape
is a positive routing decision (this operand is a boxable single object) with everything else
diagnosed, rather than an ever-growing list of rejected types.

## Related

- [[as-boxing-skips-pointer-shape-rejection]]
- [[as-boxing-skips-ownership-transfer]]
