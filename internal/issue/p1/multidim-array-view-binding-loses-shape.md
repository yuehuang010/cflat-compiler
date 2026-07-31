# Binding a MULTI-DIMENSIONAL fixed array to a view loses the row shape

Filed 2026-07-31 while fixing [[auto-binding-of-fixed-array-loses-shape]] and
[[fixed-array-copy-invalid-bitcast]]. This is the residual of that work, deliberately
deferred and NOT introduced by it: both repros below behave identically on `4097959`
and on the fix commit.

Severity: SILENT MISCOMPILE. Compiles clean, exit 0, wrong value.

## Repro 1 - the 2-D view spelling itself

```cflat
extern int main() { int[2][3] a; a[1][2]=7; int[][] v = a; printf("v=%d\n", v[1][2]); return 0; }
```

Prints `v=1`, expected `7`. The `T[][]` spelling parses and compiles but the row stride
is gone, so `v[r][c]` does not address `a[r][c]`.

## Repro 2 - `auto` over a 2-D fixed array

```cflat
extern int main() { int[2][3] a; a[1][2]=7; auto s = a; printf("s=%d\n", s[1][2]); return 0; }
```

Prints garbage (`-142573312`, varies per run) - the same never-materialised binding the
1-D `auto` case had.

## Why it was deferred rather than fixed

The 1-D fix deduces the array view `T[]` for `auto x = <fixed array>`. That deduction is
guarded on `ConstInnerDimensions.empty()` precisely because there is no correct 2-D target
to deduce TO: repro 1 shows the `T[][]` view spelling is itself broken. Deducing `T[]` for
a 2-D source would be wrong - the decayed element is a ROW, not a `T`. Fixing the `auto`
case therefore requires fixing the multi-dimensional VIEW representation first (a `T[][]`
view needs to carry the inner extent, which the thin-pointer view repr does not today).

Note the fixed-array COPY (`int[2][3] b = a;`) is correct and covered by a regression leg -
a copy needs only the total byte extent, not the row stride.

## Fix direction

Fix the `T[][]` view representation first (carry `ConstInnerDimensions` on the view so the
subscript emits the row stride), then drop the `ConstInnerDimensions.empty()` guard on the
`auto` deduction in `ParseDeclaration` (`cflat/MainListener.h`) so multi-dimensional sources
deduce the now-working view. Do NOT deduce `T[]` for a multi-dimensional source.
