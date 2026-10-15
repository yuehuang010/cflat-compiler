# Generic function names do not deduce from function-pointer arguments

## Repro

```cflat
void del<T>(move T* p) { delete p; }
q.HC<int, function<void(int*)>>(new int(1), del);
```

Passing the bare generic name to a C++ constructor parameter with a function-pointer signature
does not deduce `T`. A generic plus non-generic overload set is refused as well.

## Observed vs expected

Observed: constructor selection reports that no constructor matches arguments `('int*', '')`.
Expected: deduce `T = int`, bind `del<int>` as the `function<void(int*)>` argument, and return 1;
clang accepts the standalone C++ analogue.

## Root cause and fix direction

Generic function templates are not instantiated as candidate overloads while matching a bare
function name to a function-pointer destination. Keep this separate from non-generic overload
selection: add function-template deduction against the destination signature, including the
generic/non-generic mixed set, then feed the resulting concrete function through the existing
function-pointer binding checks.
