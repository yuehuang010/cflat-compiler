# CFlat default-parameter temporaries: remaining caller lifetime

Residue: an earlier-formal C++ class default remains in the wrapper, Round 3 C++ class-parameter
brace shapes remain clean refusals, and a later default cannot name an earlier defaulted formal.

## Earlier-formal C++ class default remains in the wrapper

The C++ class-typed default cannot be built independently at the call site when it reads an
earlier formal. Its wrapper still destroys the native temporary used to form the class default
before the caller's full expression ends. Repro in `Test/test_cpp_interop.cb`:

```cflat
int DefTmpCxxEarlier(cppdefcls.Trace first,
    cppdefcls.Trace d = cppdefcls.Trace(DefTmpCxxMk(first.value + 1).value))
{ return first.value * 10 + d.value; }
DefTmpCxxEarlier(cppdefcls.Trace(4));
```

The current leg returns 45 and leaves both native and C++ live counts at baseline; the native
temporary's wrapper lifetime remains earlier than the caller's statement lifetime.

## Round 3 C++ class-parameter brace shapes remain clean refusals

Keep the established refusal for class-typed, pointer/nullptr, string-literal, and nested-brace
elements in a C++ class parameter default. Representative shapes:

```cflat
int f(cppdefcls.Implicit d = {cppdefcls.Trace(7)});
int f(cppdefcls.Implicit d = {nullptr});
int f(cppdefcls.Implicit d = {"text"});
int f(cppdefcls.Implicit d = {{7}});
```

Scalar, non-null, non-string brace elements remain supported.

## Earlier-defaulted formal referenced by a later default

This remains the existing `Undefined variable a` diagnostic:

```cflat
int* pb(int a = 5, alias Q d = mkQ(a * 2)) { return &d.v; }
```
