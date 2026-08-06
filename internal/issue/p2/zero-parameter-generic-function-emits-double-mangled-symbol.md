# A generic function with NO value parameters links against a double-mangled symbol

Filed 2026-08-06 by `fix/sizeof-closure` (found while probing `sizeof(T)` inside a generic body -
unrelated to that fix, and not touched by it).

Severity: link failure on legal code. No wrong value, no crash.

## Repro - measured identical on `f24fb18` and on the `fix/sizeof-closure` binary

```cflat
struct P { int x = default; int y = default; };
int gid<T>() { T v = default; return 7; }
extern int main() { printf("A=%d\n", gid<P>()); return 0; }
```

```
ld64.lld: error: undefined symbol: __gid__P_gid__P__
>>> referenced by out.o:(symbol _main+0x8)
```

The mangled name is the instantiation name applied twice (`gid__P` wrapped in another
`_gid__P__`), so the call site and the definition disagree on the symbol.

## What is and is not affected - measured, both binaries

| Program | Result |
|---|---|
| `int gid<T>() { T v = default; return 7; }` called `gid<P>()` | link error, `__gid__P_gid__P__` |
| `int gsz<T>() { return (int)sizeof(T); }` called `gsz<P>()` | link error, `__gsz__P_gsz__P__` |
| `int gone<T>(int k) { T v = default; return k + 1; }` called `gone<P>(5)` | compiles, runs, prints 6 |
| `int gtwo<T>(T v) { return 9; }` called `gtwo(p)` (inferred) | compiles, runs, prints 9 |

So it is the ZERO-value-parameter generic function called with an explicit type argument. One
value parameter is enough to make the same shape link.

## Root cause

Not diagnosed. The two working rows show the normal instantiation path produces the right symbol,
so the suspicion is a second mangling applied on the explicit-type-argument call path when the
argument list is empty (the arity-0 call is the only thing that distinguishes the failing rows).
Start at the generic-function call-site naming in `MainListener.h` and compare the arity-0 branch
with the arity-1 one.

## Test coverage

None.

Related: [[interface-issue-queue]]
