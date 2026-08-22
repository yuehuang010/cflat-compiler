# Brace-init of an owning STRUCT field from a temporary is a silent use-after-free

Filed 2026-08-21 while working the p3 string-from-a-temporary issue. **Pre-existing on master
(`031aefe`) and unchanged by that work** - measured on both binaries.

This is the SYNTAX-AXIS twin of the p1 borrow-from-temp-escapes-into-struct-field fix: the `=`
field store rejects, the brace-init member spelling of the same store does not.

## Repro

```cflat
import "string.cb";
import "list.cb";
struct Tok { string text = default; };
struct Outer { Tok inner = default; };
struct Holder { Tok slot = default; };
Holder makeHolder() {
    list<Outer> outers;
    Outer o = default; o.inner.text = "abcdefghijklmnop" + "qrstuvwxyz"; outers.add(o);
    Holder h = { slot = outers.get(0).inner };   // ACCEPTED - should be rejected
    return h;
}
extern int main() {
    Holder kept = makeHolder();
    printf("out=%s\n", kept.slot.text.data());
    return 0;
}
```

Compiles clean, exits 0, and prints an empty/garbage string on both binaries (measured empty on
both; `out=2\xef\xbf\xbd...` when run under MallocScribble=1) - a silent read of the freed list
buffer.

The `=` spelling of the same store IS rejected:

```
h.slot = outers.get(0).inner;
-> cannot store 'Outer.inner' taken from a temporary into a longer-lived location; ...
```

## Root cause (hypothesis, not yet confirmed in IR)

`MainListener::EmitOneFieldInit` (cflat/MainListener_Expressions.cpp) carries the brace-path
copies of the `=` path's ownership rules for `unique` fields, closures, interfaces and `string`,
but has no leg for `rightNV.FromOwningTempField` on an owning VALUE-type field. The `=` twin's
leg is the `FromOwningTempField && IsOwningValueType(TypeName)` reject in
`MainListener_Expressions.cpp` (~2576).

## Fix direction

Add the missing brace leg next to the `string` one in `EmitOneFieldInit`: when
`rightNV.FromOwningTempField && !rightNV.MovableTempField &&
compiler->IsOwningValueType(fieldType.TypeName)`, reject with the `=` path's wording. Note the
`string` case must NOT be folded in - it binds by implicit copy (see doc/LANGUAGE.md,
"A `string` read out of a temporary").

Add the accept/reject leg to `Test/errors/err_temp_field_store.cb`, which already carries the
`=`, indexed, nested, pointer-destination and method spellings of this reject.
