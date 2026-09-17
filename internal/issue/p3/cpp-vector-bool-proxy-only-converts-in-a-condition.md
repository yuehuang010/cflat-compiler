# `std.vector<bool>` element converts in an `if` but not in an initializer, and the diagnostic is the wrong one

Bucket: **p3** (usable-surface gap plus a misleading diagnostic; no wrong values).

## Summary

`std::vector<bool>` returns a proxy (`std::__bit_reference`) from `operator[]`. CFlat converts
that proxy correctly in a CONDITION - `if (v[0])` gives the right answer - but rejects the same
expression in an initializer or an explicit cast, and the message it prints for the initializer
is the CONDITION message:

```
bb4_vb4.cb(6,13): the condition must be a single value (bool, integer, pointer, or floating
point), not 'std.__bit_reference$std.vector$bool$std.allocator$bool$true' - compare it explicitly
```

There is no condition on that line. Two further problems in that one line of output:

- the type is printed in RAW `$` MANGLED FORM, not demangled. The invertible-mangling ruling is
  that every user-facing surface demangles; this surface does not.
- the advice ("compare it explicitly") does not help: `v[0] == true` has the same problem, and
  the type named cannot be spelled in CFlat anyway.

The cast form produces a different, better-worded message but the same refusal:

```
bb4_vb.cb(7,47): cannot cast 'std.__bit_reference<std.vector<bool<std.allocator<bool>>>, true>'
to 'int'; no 'operator int' defined for ...
```

- note the mis-rendered specialization `std.vector<bool<std.allocator<bool>>>` - the comma
  between template arguments has been turned into nesting.

## Repro

`scratch/bb4_vb4.cb` (refused, reproduced twice):

```cflat
import cpp "vector" cache;
extern int main()
{
    std.vector<bool> v = default;
    v.push_back(true);
    bool b = v[0];               // refused with the "condition" message
    printf("b=%d\n", (int)b);
    return 0;
}
```

`scratch/bb4_vb3.cb` (compiles, correct values):

```cflat
std.vector<bool> v = default;
v.push_back(true); v.push_back(false);
if (v[0]) ... // if0=1
if (v[1]) ... // if1=0
```

## Fix direction

Whatever conversion the condition path applies to `std.__bit_reference` (it must be finding
`operator bool()`) should be applied by the ordinary implicit-conversion path used by
initializers, assignments and explicit casts, so `bool b = v[0];` and `(int)v[0]` work.

Separately, and independent of the `vector<bool>` question:
- the initializer path must not reuse the condition diagnostic - it should say the source type
  cannot convert to the destination type and name both;
- demangle the type in that message (`std.__bit_reference<...>`), per the mangling ruling;
- fix the specialization renderer that turns `std.vector<bool, std.allocator<bool>>` into
  `std.vector<bool<std.allocator<bool>>>`.
