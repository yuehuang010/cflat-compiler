# `new T(args)` on a trivially copyable C++ class never calls the constructor

Found 2026-09-22 while measuring the constructor-template matrix. SILENT WRONG VALUE.

## Repro

```cflat
import cpp "tct.h";   // struct NoTpl { int v; int how; NoTpl(int a, int b) : v(a + b), how(9) {} };
extern int printf(char* f, ...);
extern int main()
{
    tct.NoTpl* a = new tct.NoTpl(2, 3);
    printf("%d %d\n", a->v, a->how);   // prints "0 0"; expected "5 9"
    return 0;
}
```

No constructor template involved. Same result for any trivially copyable class with a user
constructor (tested: plain, template, enable_if and requires constructors). `tct.NoTpl x =
tct.NoTpl(2, 3)` is correct.

## Root cause (measured)

`MainListener_Expressions.cpp`, the `new` path: the C++ branch ("M4b - `new T(args)` on a foreign
NONTRIVIAL C++ class") is gated on `IsForeignNontrivialCxxClass(typeName)`. A trivially copyable
record is not in `cxxNontrivialRecords_`, so it takes CFlat's own `new`, which zero-fills and looks
for a CFlat constructor - the arguments are evaluated and dropped without a diagnostic.

## Fix direction

Gate the C++ branch on `IsForeignCxxClassWithConstructors` (or at least on "has any user
constructor") rather than on non-triviality; allocation can stay CFlat's for trivial classes, but
construction must go through SelectCxxConstructor / the constructor-template wrapper. Acceptance:
the repro prints `5 9`; value legs in Test/test_cpp_interop.cb next to `new cppi.Tracked(13)`.
