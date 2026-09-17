# A `consteval` C++ function binds like an ordinary function and fails with a raw linker error

Bucket: **p3** (missing diagnostic; no miscompile, but the error a user sees is a mangled-name
linker dump instead of a compiler message).

## Summary

`consteval` functions have no runtime address - clang emits no symbol for them. CFlat binds one
as a normal external function and emits a call, so the failure surfaces at link time as an
`Undefined symbols for architecture arm64: "bb4c::must_ce(int)"`, with the C++ mangled spelling
and no source location. The user is given no hint that `consteval` is the problem.

The neighbouring cases are all correct: a `constexpr` function called at runtime works, an
`inline constexpr` variable of class type works, `static inline` and `static constexpr` members
work.

## Repro

`scratch/bb4_ce.h` (excerpt):

```cpp
#pragma once
namespace bb4c {
constexpr int square(int n) { return n * n; }
consteval int must_ce(int n) { return n + 1; }
}
```

`scratch/bb4_ce2.cb`:

```cflat
import cpp "bb4_ce.h";
extern int main()
{
    printf("ce=%d\n", bb4c.must_ce(4));
    return 0;
}
```

Measured (macOS arm64, Release, master 2798eb1a), reproduced twice:

```
Undefined symbols for architecture arm64:
  "bb4c::must_ce(int)", referenced from:
      _main in bb4_ce2.out.o
ld: symbol(s) not found for architecture arm64
Error: linking failed (exit 1):
Error: failed to emit executable 'scratch/bb4_ce2.out'.
```

For contrast, `scratch/bb4_ce.cb` compiles and prints `sq=36`, `sqlit=25`, `org=1,2`, `cnt=11`,
`k=3` - so only `consteval` is affected.

## Fix direction

The clang AST dump already carries the `consteval` bit on `FunctionDecl`
(`isConsteval`/`ConstexprSpecKind::Consteval`). Two options, in preference order:

1. Fold the call at compile time - CFlat already evaluates C++ `constexpr` VARIABLES, so a
   `consteval` call with constant arguments could be evaluated through clang and folded. This is
   the behaviour C++ users expect.
2. Failing that, refuse the binding with a message in the existing style, e.g.
   `'bb4c.must_ce' was not bound: 'consteval' functions have no runtime symbol`, so the error
   appears at the call site with a file(line,col) prefix instead of at link time.

Option 2 is a small change in the same place the other "was not bound" refusals are produced and
should land regardless of whether 1 is ever done.
