# `for (T x in v)` over a `T[]` VIEW does not compile

Filed 2026-08-10 from the fix/forinstr round. Pre-existing and unchanged: identical diagnostic
on `01853aa` and on fix/forinstr (scratch/fi_c_view.cb).

Severity: missing feature, loud compile error, no miscompile.

## Repro

```cflat
extern int main()
{
    string[2] dst;
    dst[0] = "ab" + "cd";
    dst[1] = "ef" + "gh";
    string[] v = dst;                     // T[] view
    int n = 0;
    for (string s in v) n += s.length();  // does not compile
    printf("n=%d\n", n);
    return 0;
}
```

Diagnostic on both binaries:

```
fi_c_view.cb(8,21): no overload of 'count' matches the given arguments.
  Call arguments (1):
    [0] string* <this>
  Candidates (1):
    _count_i32_list__stringPtr_(list__string* list__string__)
```

## Root cause

The range-`for` lowering in `MainListener_Statements.cpp` has three legs: `isFixedArray` (an
`llvm::ArrayType` base type), `isFaceType`, and an `else` that resolves user `count()` / `get()`
overloads. A `T[]` view is neither an `ArrayType` nor an interface, so it falls into the `else`
leg and looks for a `count(string*)` overload that does not exist - hence a diagnostic about
`list<string>`, which is confusing and names a type the program never mentions.

## Fix direction

A view carries a pointer and a length, so it has everything the counted lowering needs. Add a
fourth leg beside `isFixedArray`: take the length from the view's length slot and index the
element with a SINGLE-index GEP. When adding it, give the element read the same owned-bit
clearing the fixed-array leg now has (`ClearStringOwnedBit` + `ClearStructOwnedBits`), or the
new leg will land the exact double free this round removed - a view's slots are LIVE, which is
the whole finding of `01853aa`. If a fourth leg is not wanted, at minimum improve the
diagnostic: a `T[]` receiver should say range-`for` over a view is unsupported rather than
report a missing `list<T>` overload.
