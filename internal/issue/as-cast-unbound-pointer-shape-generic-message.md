# `as` on a pointer-shaped source with no named binding gives the generic message

Filed 2026-07-28 by the round-2 and round-3 reviews of the `as`/`is` source-routing fix.

Severity: DIAGNOSTIC QUALITY only. Both shapes below are correctly REJECTED - the
wording is just the classifier's generic fallback rather than the shared
`RejectPointerShapedInterfaceUpcast` message the plain spelling gives. No miscompile.
On the pre-fix binary both were accepted silently (exit 0), so this is a residual of an
improvement, not a regression.

## Repro

```cflat
interface IShape { int area(); };
class Circle : IShape { int r = 0; int area() { return r * r; } };
struct H { Circle** pp = default; };

int f(H h)          { IShape s = h.pp as IShape; return 0; }   // struct FIELD
extern int main()   { Circle*[3] arr; IShape s = arr as IShape; return 0; }  // LOCAL array of pointers
```

Both produce:

```
'as' requires an interface value or a class instance on the left of 'IShape'; this
expression is neither
```

The plain spelling of each produces the shared message instead:

```
cannot convert 'Circle**' to interface 'IShape' - only a single instance pointer
'Circle*' or a 'Circle' value can be boxed into an interface fat pointer; index or
dereference it first
```

Note the GLOBAL spelling of the array-of-pointers case DOES get the shared wording, so
the two differ only in storage class. That inconsistency is the strongest argument for
fixing this.

## Root cause

`ClassifyPointerShapedSource` (`MainListener.h:12181-12207`) recovers the binding's
declared `TypeAndValue` through `FindDeclaredTypeAndValueForStorage`
(`LLVMBackend.h:2765-2796`), which is keyed on STORAGE. It resolves a local (alloca),
a parameter, and a global (`GlobalVariable` matched by identity, type fetched by name
from `globalVariableTypes`).

Neither shape above has a storage key to look up:

- a struct field loads through a `getelementptr`, not a named binding's slot;
- a local array of pointers decays to a `getelementptr` as well, so `storage` is never
  set and the lookup is never reached.

With no declared type there is nothing for `DescribePointerShapedInterfaceSource` to
name, so the code correctly falls back to the generic message rather than inventing one.

## Fix direction

Recover the declared type for a GEP-derived source by walking the GEP back to its base
pointer and looking THAT up - the base of `h.pp` is the `H` binding, and the base of a
decayed local array is the array's own alloca. Then describe the field/element type from
the struct layout in `dataStructures` rather than from a variable binding.

This is the same "the boxing site should record what it boxed rather than recovering it by
walking IR" argument that drove the boxing consolidation, which has since landed:
`BoxConcreteIntoInterface` (`MainListener.h:9969`) now records provenance per box. That does
NOT close this issue - the shapes here fail EARLIER, at the point of recovering a declared
type to name in the message, before any boxing record exists. But it means the recovery
should be written once, in the helper, rather than as a third walk bolted onto the
classifier. See [[interface-boxing-sites-not-fully-consolidated]].

## Guard rail

`Test/errors/err_as_array_source_interface.cb` documents the exact scope of the
byte-identical-wording guarantee and names both of these as known exceptions. If this
issue is fixed, update that comment and add legs for both shapes.

## Related

[[interface-boxing-sites-not-fully-consolidated]], [[interface-issue-queue]]
