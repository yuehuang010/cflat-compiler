# CFlat alias of a C++ class template used as a constructor

The original T458 empty-list findings were closed in this round or did not
reproduce on current HEAD/master. The one remaining note is the separate CFlat
alias case below.

## Repro

```cflat
import cpp "cpp_interop_tpl.h";
using G = cppt.dflt.Gate;
extern int main()
{
    G value = G(1);
    return value.arrive();
}
```

Current HEAD/master reports `the function 'G' is not known`. The explicit empty
form `G<> value = G<>(1);` reports `unknown generic function 'G'`.

## Root cause

This is a CFlat type alias naming a C++ class template base. The failure occurs
while resolving the CFlat alias as a constructor call. It does not use the C++
alias-template pattern path changed for T458, and the type is not a native
CFlat non-generic declaration.

## Fix direction

Resolve a CFlat type alias that names a C++ class template to its class-template
constructor path before function lookup. This remains separate from empty
C++ alias-template argument validation.
