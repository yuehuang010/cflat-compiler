# 'auto' bound to a fixed array loses the array shape

Filed 2026-07-29 while fixing `global-primitive-array-boxed-into-interface`. PRE-EXISTING
and unrelated to that fix: identical on `df32dd8` and on the fix commit.

Kept as its own file rather than folded into the primitive-array work: the defect is in the
`auto` BINDING, and the interface symptom below is only the most visible consequence of it.
The interface guards are not at fault - they are handed a binding that no longer says
"array", which is exactly the information they decide on.

Severity: two symptoms, one SILENT MISCOMPILE.

## Repro 1 - silent miscompile, no interface involved

```cflat
int[3] gInt;
extern int main() { gInt[0] = 5; auto s = gInt; printf("%d\n", s[0]); return 0; }
```

Both binaries: compiles clean, exit 0, prints garbage (`12796552`, varies per run). The
whole emitted body is:

```llvm
store i32 5, ptr @gInt, align 4
call void (ptr, ...) @printf(ptr nonnull @2, ptr nonnull @gInt)
```

`s[0]` did not index anything - the ARRAY POINTER itself was passed as the `%d` argument.
The local `s` was never materialised.

## Repro 2 - the interface consequence

```cflat
interface IShape { int area(); };
int[3] gInt;
extern int main() { auto s = gInt; IShape t = s; return 0; }
```

Both binaries, exit 1, with no source diagnostic:

```
Module verification failed:
Invalid bitcast
  %1 = bitcast ptr %0 to %__iface_fat_ptr
```

Written WITHOUT the `auto` intermediate, the same program is rejected cleanly on the fix
commit: `cannot convert 'int[3]' to interface 'IShape' - 'int' is a primitive type and can
never implement an interface`. So an `auto` intermediate defeats the guard.

## Root cause

Partly diagnosed. The guard added in the primitive-array fix decides from the source's
declared `TypeAndValue` and rejects only when it is provably pointer-SHAPED
(`ConstArraySize`, `IsArrayView`, `ElemPointer`, `IsSimd`). The `auto` binding carries none
of those, so the guard correctly declines to reject something it cannot prove - accepting is
the intended polarity. What is actually wrong is upstream: `auto` deduces a shape from a
fixed-array initializer that is neither the array nor a usable view, and repro 1 shows the
binding is not even indexable. Where exactly the deduction drops the extent is NOT
diagnosed.

## Fix direction

Fix the deduction, not the guard. `auto x = <fixed array>` should deduce either the array
view `T[]` (the spelling that already works everywhere) or the array type itself; whichever
is chosen, `x[i]` must index and the binding must carry the shape fields. The interface
symptom then disappears on its own, because the guard will see a pointer-shaped source
again. Do NOT "fix" this by widening the interface guard to reject un-shaped sources - that
is precisely the accept-everything-unproven polarity the guard exists to preserve.

## Related

[[interface-issue-queue]]
