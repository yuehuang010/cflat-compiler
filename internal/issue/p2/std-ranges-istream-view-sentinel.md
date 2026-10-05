# `std.views.istream<int>(input)` cannot be bound (variable-template CPO spelling)

NARROWED 2026-10-03: `for (int x in std.ranges.istream_view<int>(input))` now works (default_sentinel_t
end; std_full_20_907 re-enabled, found passing during T19). The `views::istream<T>` spelling still fails in
name lookup.

## Repro

```cflat
import cpp {"ranges", "sstream"};
extern int main()
{
    std.istringstream input = std.istringstream("4 5 6");
    i64 sum = 0; for (int x in std.views.istream<int>(input)) sum += x;
    return sum == 15 ? 0 : 1;
}
```

Expected: exit 0 (clang++ -std=c++20 twin sums 15). Actual:
`'istream' is not a member of namespace 'std.istream.std' (C++ free function 'std.istream.std.istream'
could not be bound (clang: 'std' is not a class, namespace, or enumeration))`

## Root-cause guess (unverified)

`std.views.istream` resolves `istream` as the `std::istream` typedef first and re-roots the lookup under it;
`views::istream<T>` is a variable template (a customization-point object), the same family as
p2/std-ranges-empty-view.md (`views::empty<T>`).

## Fix direction

Resolve a templated name under a C++ namespace path as a variable template before falling back to a
same-named type in an enclosing namespace; bind the CPO object and call its operator().

Found by: test_libs/std_full/std_full_20_ranges (2026-10-02); narrowed 2026-10-03.
