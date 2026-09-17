# A C++ alias template names a type in a declaration but is "not known" as a constructor

Found 2026-09-17 by the C++-interop bug bash round 5 (macOS arm64, Release, worktree at master
fcdabd92).

Bucket: **p3** (surface inconsistency plus a wrong-category diagnostic; the workaround -
`= default`, or spelling the aliased template - is complete). Distinct from round 4 cell 21,
which covered a NON-template alias (`using V = std::vector<int>;`, fully working).

## Summary

For `template<class T> using Vec = std::vector<T>;`, the declaration
`bb5.Vec<int> v = default;` binds and works, and a C++ parameter typed `const Vec<int>&` accepts a
`std.vector<int>` argument. But the constructor spelling `bb5.Vec<int>()` is refused with
`the function 'Vec' is not known.` - a FUNCTION diagnostic for a type name the very next line
resolves.

## Repro

`scratch/bb5_tpl.h`:

```cpp
#pragma once
#include <vector>
namespace bb5 {
template<class T> using Vec = std::vector<T>;
inline int vsum(const Vec<int>& v) { int s = 0; for (int x : v) s += x; return s; }
}
```

`scratch/bb5_tpl.cb` - refused, twice:

```c
import cpp "vector" cache;
import cpp "bb5_tpl.h";
extern int main()
{
    bb5.Vec<int> v = bb5.Vec<int>();
    ...
}
```

```
bb5_tpl.cb(6,21): the function 'Vec' is not known.
```

`scratch/bb5_tpl3.cb` - the same type, `= default`:

```c
    bb5.Vec<int> v = default;
    v.push_back(5);
    printf("n=%d expect=1\n", (int)v.size());
```

```
n=1 expect=1
run=0
```

`scratch/bb5_tpl2.cb` - the alias as a C++ PARAMETER type, given a `std.vector<int>`:

```
aliasparam=7 expect=7
run=0
```

## Root cause

Not investigated. The declaration path resolves `bb5.Vec<int>` through the type request (clang
desugars the alias), while the call path looks the name up in the function/constructor tables
only, where an alias template has no entry - hence the "function" wording. The constructor path
needs the same alias desugaring the declaration path already performs.

## Fix direction

Desugar an alias-template spelling to its underlying specialization before constructor lookup, so
`bb5.Vec<int>()` becomes `std.vector<int>()`. If that is not wanted, refuse it as a TYPE ("`Vec`
is a C++ alias template; spell the aliased type, or use `= default`") rather than as an unknown
function. Fixture: extend the `Test/library/cpp_interop_tpl.h` alias rows.
