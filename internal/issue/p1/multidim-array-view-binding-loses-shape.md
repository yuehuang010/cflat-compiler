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

## Repro 3 - the PARAMETER axis (measured 2026-08-02 on `ca5a02a`)

```cflat
int f(int[][] v){ return v[1][2]; }
extern int main(){ int[2][3] a; a[1][2]=7; printf("p=%d\n", f(a)); return 0; }
```

Compiles clean, exit 0, prints `p=1` - expected `7`. Same silent wrong value as repro 1, so a
`T[][]` PARAMETER is a live defect and not merely a theoretical consequence of the view repr.
This matters for scoping: rejecting `T[][]` parameters outright is a legitimate scope cut (a
thin-pointer view cannot express a row stride), but it is a real behaviour change on a shape that
compiles today, not a no-op.

Note the counterpart `char[8]` FIXED-extent parameter does not even bind - `int f(char[8] b)`
called with a `char[8]` gives `no overload of 'f' matches the given arguments` with the argument
shown as `[0] ptr <unnamed>`. So the parameter axis is inconsistent across the two spellings: the
view spelling silently miscompiles, the fixed-extent spelling false-rejects.

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
