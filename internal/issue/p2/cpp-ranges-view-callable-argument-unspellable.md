# C++20 ranges view adaptors cannot be bound from CFlat

Summary: `std.views.filter(fn)` fails to bind even with a `std.function` argument (the spelling that
works for `erase_if`, `visit`, `jthread`): the adaptor's return type
`std::ranges::__pipeable<std::__bind_back_t<...>>` is not bindable, so the range-adaptor closure
cannot be produced or composed with `|`. With a CFlat native `function<>` argument the earlier
error is p2/cpp-cflat-callable-to-deduced-template-param.md.

Repro:
```cflat
import cpp {"ranges", "functional"};
bool is_even(int v) { return v % 2 == 0; }
extern int main()
{
    std.function<bool(int)> p = std.function<bool(int)>(is_even);
    auto sel = std.views.iota(0, 8) | std.views.filter(p);
    int n = 0;
    for (auto it = sel.begin(); it != sel.end(); it++) n += *it;
    return n == 12 ? 0 : 1;
}
```

Observed: `'filter' is not a member of namespace 'std.views' (C++ free function 'std.views.filter'
could not be bound (clang: '__cflat_free_...' was not bound: return type
'std::ranges::__pipeable<std::__bind_back_t<std::ranges::views::__filter::__fn, std::tuple<std::function<bool (int)>>>>' ...`
Expected: the closure binds, `|` composes it with `iota`, iteration yields 0+2+4+6 = 12.

Disabled case: test_libs/std/std_20_94_ranges_views.cb (filter | transform | take).
