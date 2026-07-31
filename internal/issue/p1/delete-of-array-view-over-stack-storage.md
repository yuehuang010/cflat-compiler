# 'delete[_]' on an array view over stack storage aborts with no diagnostic

Filed 2026-07-31 while fixing [[auto-binding-of-fixed-array-loses-shape]]. PRE-EXISTING and
NOT caused by that fix: the explicit view spelling below behaves identically on `4097959`
and on the fix commit.

Severity: silent abort (exit 134), no diagnostic at all. Per CLAUDE.md's convention an
abort like this should be a proper compiler error.

## Repro - identical on both binaries

```cflat
extern int main() { int[3] a; int[] v = a; delete[_] v; return 0; }
```

Compiles clean, then aborts at runtime (exit 134) - `free()` is handed a stack address.

## Why it is filed now

The fixed-array shape fix makes `auto s = a;` deduce the view `int[]`, so `delete[_] s;`
now reaches this same hole through one more spelling. On master that spelling was rejected
("cannot 'delete' value-type local 's' of type 'auto'"), but that rejection was an accident
of the BROKEN binding - `s` had no shape at all, so it looked like a value type. It was
never a real safety check, and the explicit `int[] v = a;` spelling always got through.

## Root cause

Not diagnosed. A `T[]` view is a thin `T*`, so the delete path cannot tell a view over
`new T[n]` from a view over a fixed array's decayed storage. The information IS available at
the binding site (the RHS carries `ConstArraySize`), it is just not carried onto the view.

## Fix direction

Record at the binding site whether a view was bound from a heap allocation (`new T[n]`) or
from stack/global fixed-array storage, and reject `delete` on the latter with a LogError
naming the local and where its storage came from. Do NOT reject `delete` on views whose
origin cannot be proven - the guard must reject only what it can prove, matching the
polarity of the array-view bind gate (`RejectRawPointerToArrayView`).
