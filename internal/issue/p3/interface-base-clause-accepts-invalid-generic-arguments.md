# Interface base clause accepts invalid generic arguments

The widened base-clause grammar accepts type arguments on an interface parent, but the
interface-parent path drops them before lookup. That silently turns invalid source into a
different declaration instead of diagnosing it.

## Repro

```cflat
interface IBase { int base(); };
interface IDerived : IBase<int, float, whatever> { int derived(); };
```

The source should either instantiate a generic parent with checked arguments or reject the
generic argument list. In the current implementation `BaseSpecifierName` keeps only `IBase` and
the declaration is accepted.

## Fix direction

Validate and resolve the complete base specifier. If generic interface inheritance is implemented,
substitute and mangle the parent instance; otherwise emit a direct diagnostic that generic
arguments are not supported on this parent. Coordinate with
`p2/generic-interface-cannot-inherit-generic-interface.md` before changing the shared path.
