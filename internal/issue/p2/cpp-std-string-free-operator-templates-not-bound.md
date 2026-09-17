# Free operator templates for std.string are not bound

## Summary

The libc++ free operator templates for `std.string` are not usable from CFlat. Binary
operators such as `+`, `==`, and `<` fail even when both operands are real `std.string`
objects. This is separate from the string-literal operator fix under review.

## Repro

```cflat
import cpp "string" cache;
extern int main()
{
    std.string s = std.string("hello");
    std.string s2 = std.string(" world");
    std.string t = s + s2;
    return (int)t.size() == 11 ? 0 : 1;
}
```

Observed on 2026-09-16 with `scratch/cflat-prefix` and `x64/Release/cflat`:

```text
strop_x1.cb(6,19): no operator '+' for type 'std.string'
```

Both binaries exit 1 with the same diagnostic. The same limitation is reported for the
free `==` and `<` operators in the companion probes.

## Root cause

The libc++ declarations are free function templates, but the C++ interop binding path does
not register or instantiate them for these operands. The candidate set therefore contains
no usable free operator even when both operands are already `std.string`; this is not a
literal-decay or argument-ranking failure.

## Fix direction

Make the C++ free-function binding path request and instantiate the public `std.string`
operator templates for concrete operands, including `+`, `==`, `!=`, `<`, `>`, `<=`, and
`>=`. Add value assertions for the operators and keep this work separate from literal
decay into `const char*`.
