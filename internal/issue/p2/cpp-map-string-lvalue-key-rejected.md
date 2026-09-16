# A named `std.string` lvalue cannot be used as a `std.map` key

## Summary

`std.map<std.string, int>` works with a string literal key, but a named `std.string`
lvalue is rejected by `operator[]`. This blocks the normal dynamic-key form of a word
counter or lookup table. Found 2026-09-16 during the standard-library dogfood session.

## Repro

```cflat
import cpp { "string", "map" } cache;

extern int main()
{
    std.map<std.string, int> counts = default;
    std.string key = std.string("compiler");
    counts[key] = counts[key] + 1;
    return 0;
}
```

The first compile of `scratch/dogfood/std/word-count.cb` reported:

```
word-count.cb(15,4): no overload of 'operator[]' matches the given arguments.
  Call arguments (2):
    [0] std.map<std.string, int> <this>
    [1] %std.string = type { [3 x i64] } <unnamed>
  Candidates (6):
    ...
    operator[](std.map<std.string, int>*, std.string)
    operator[](std.map<std.string, int>*, std.string*)
```

`counts["compiler"]` compiles and the complete word-count program runs, but that
workaround cannot handle a runtime-created word without first converting it back to a
literal, which is impossible in a real parser.

## Root cause (hypothesis)

The named foreign class local reaches C++ member overload resolution as an unnamed
aggregate value instead of retaining its addressable `std.string` lvalue identity. The
correct `const std.string&` / `std.string&&` candidates are present, but neither matches
the value shown in the diagnostic. The precise source path was not traced.

## Fix direction

Preserve the foreign class local's lvalue/rvalue category and storage through template
member calls, then bind the matching `std.map::operator[]` reference overload. Extend the
interop fixture with a `std.map<std.string, int>` dynamic-key row and a lookup using a
named key, not only string literals.
