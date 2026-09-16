# A field whose type is a C++ class-template specialization is embedded as opaque bytes

Found 2026-09-16 while fixing the alias-of-specialization issues (cell `b06`/`b08` of that matrix).
Not alias-specific: the alias spelling and the specialization's own spelling behave identically.

## Repro

Header:

```cpp
namespace refns {
    template <class T> struct RefTpl { T v; };
    struct HoldsSpec { RefTpl<int> h; };        // same with `using A = RefTpl<int>; A h;`
}
```

CFlat:

```cflat
import cpp "alias_tpl2.h";
extern int main() { refns.HoldsSpec h = default; h.h.v = 6; if (h.h.v != 6) return 1; return 0; }
```

    alias_b08.cb(2,53): Undefined variable v.

`--check -v` says why:

    C++ struct 'refns.HoldsSpec': field 'h' of type 'refns::RefTpl<int>' embedded as 4 opaque bytes

## Root cause

Not established. The enclosing record is registered before the field's specialization is requested,
so the field lands as opaque storage and its members never resolve. Measured identical on the
pre- and post-fix binaries of `fix/cpp-alias-types`.

## Fix direction

Request the specialization named by a field type before laying out the enclosing record, as the
member-type request already does for named classes.
