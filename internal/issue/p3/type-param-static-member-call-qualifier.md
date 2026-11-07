# Type template parameter as a qualifier: `T.sm()` unresolved, namespace `T` wins over the parameter

## Summary

Inside a generic body a TYPE parameter cannot qualify a static member call, and a namespace with the parameter's
spelling is preferred over the parameter. C++ (`T::sm()`) resolves `T` to the template argument in both cases.

## Minimal repro

```cflat
struct S { static int sm() { return 9; } };
int f<T>() { return T.sm(); }
extern int main() { printf("%d\n", f<S>()); return 0; }
```

Observed (master c98047df): "Undefined variable T." Expected: prints 9 (clang++ -std=c++20 `T::sm()` = 9).

```cflat
namespace T { int sm() { return 1; } }
struct S { static int sm() { return 9; } };
int f<T>() { return T.sm(); }
struct Box<T, int N> { int m() { return T.sm() * 10 + N; } };
extern int main() { Box<S, 2> b = default; printf("%d %d\n", f<S>(), b.m()); return 0; }
```

Observed: `1 12` (namespace T). Expected: `9 92` - the template parameter hides the namespace, as for value parameters
since the generic-value-param-shadowing landing (box 10, 2026-11-03).

## Fix direction

The `.` qualifier lookup inside a generic body should consult the active TYPE binding first (resolved struct), then
namespaces/globals. Found by the genns round-2 review (box 10, 2026-11-03).
