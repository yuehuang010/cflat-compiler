# An `explicit` C++ constructor is used implicitly at a call argument, silently

Found 2026-09-17 by the C++-interop bug bash round 5 (macOS arm64, Release, worktree at master
fcdabd92).

Bucket: **p3** (accepts-invalid with no diagnostic; the value produced is what the ctor computes,
so nothing is miscompiled - but CFlat is more permissive than C++, and `explicit` is the author's
guard against exactly this call). Polarity is the OPPOSITE of
`internal/issue/p3/cpp-implicit-ctor-conversion-at-call.md`, which is about a NON-explicit
conversion not being offered.

## Summary

Passing an `int` to a C++ parameter of class type whose only viable constructor is declared
`explicit` compiles clean and runs the constructor. The same C++ code is ill-formed
(`error: no matching function for call to 'take_ex'`). The two other spellings of the same
conversion are refused, so the hole is specific to call arguments.

## Repro

`scratch/bb5_ctor.h`:

```cpp
#pragma once
namespace bb5c {
struct Ex { int v; explicit Ex(int a) : v(a) {} };
inline int take_ex(Ex e) { return e.v; }
}
```

`scratch/bb5_ctor3.cb`:

```c
import cpp "bb5_ctor.h";
extern int main()
{
    printf("ex=%d expect refusal or 5\n", bb5c.take_ex(5));
    return 0;
}
```

Measured (`x64/Release/cflat scratch/bb5_ctor3.cb -i scratch -o scratch/bb5_ctor3.out`), twice:

```
compile=0
ex=5 expect refusal or 5
run=0
```

Contrasts that DO refuse:

- `bb5c.Ex e = 5;` (`scratch/bb5_ctor5.cb`):
  `cannot store a single scalar value into struct storage - a fixed array or struct is not
  assignable from one value. ...`
- an `explicit operator bool` read into an int (`scratch/bb5_ctor6.cb`, class `bb5o.N` from
  `scratch/bb5_op.h`): `cannot cast an aggregate value - ...`. The conversion operator's
  `explicit` IS honoured in `if (a)` (allowed) versus `int n = a;` (refused), so the conversion-
  operator direction behaves; only the constructor direction leaks.

## Root cause

Not investigated. The C++ implicit-conversion-at-argument path built for
`cpp-implicit-ctor-conversion-at-call` selects a single-argument constructor of the parameter type
without consulting the constructor's `explicit` flag. Worth checking whether the harvested
constructor record even carries an `isExplicit` bit - if it does not, the flag has to come out of
the clang AST dump first.

## Fix direction

Exclude `explicit` constructors from the one-step implicit-conversion candidate set at a call
argument (clang's rule), and keep them reachable through the spelled form `bb5c.Ex(5)`. The
diagnostic should name the ctor and say it is explicit, pointing at the spelled form. Add a row
to `Test/errors/err_cpp_*.cb` with `expect_error` on the implicit call and a positive row in
`Test/test_cpp_interop.cb` for the spelled one.
