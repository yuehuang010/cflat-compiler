# Constructor overload resolution misses constrained/templated constructors

Found 2026-09-09 by the std header coverage spike ([`std-header-coverage-spike.md`](std-header-coverage-spike.md), gap 4).
Deepest of the eight. The type registers and `= default` works; only the explicit constructor call
fails.

## Repro

```cflat
import cpp "complex";
extern int main() { double a = 1.0; double b = 2.0; std.complex<double> c = std.complex<double>(a, b); return 0; }
```

    C++ class 'std.complex$double' has no constructor whose parameter types match these arguments ('double', 'double')

The arguments are exactly the parameter types. Same shape, all rejected:

- `std.chrono.milliseconds(n)` with `long long n` -> no constructor matching `('i64')`
- `std.filesystem.path(s)` with `std.string s` -> no constructor matching `('std.string')`
- `std.filesystem.path("a/b.txt")` -> no constructor matching `('char*')`
- `std.tuple<int, int>(1, 2)` -> no constructor matching `('int', 'int')`

Not a float-literal-default artifact: the `complex` repro binds `double` locals first and still
fails.

## Related but possibly a different root cause

For some types the constructor-call NAME does not resolve at all, before any ranking happens:

- `std.optional<int>(7)` -> `'optional' is not a member of namespace 'std'`
- `std.pair<int, double>(1, 2.5)` -> `'pair' is not a member of namespace 'std'`
- `std.regex("a+")`, `std.mt19937(1u)` -> `Undefined variable std.`

Both `optional` and `pair` declare fine as `= default`, so the TYPE is registered and only the
ctor-call spelling breaks - and `std.tuple` in the identical position DOES resolve its name and
reaches the ranking error above. Decide early whether this is one bug or two; if it is two, split
this file.

## Root cause

Not established. The common factor in the ranking failures is that the constructor is a
constrained or templated member (`template<class Rep2> constexpr explicit duration(const Rep2&)`,
`path`'s `Source`-deduced ctor, `tuple`'s conditionally-explicit ctor). `std.string("hello")`
works today and is a member template with an all-defaulted SFINAE parameter, so the path is
partly there - start by diffing why that one ranks and these do not. `explicit` and
by-const-reference parameters are both worth suspecting.

## Fix direction

Extend C++ constructor candidate collection and ranking to constrained/templated constructors,
following the `std.string` case that already works.

Acceptance: all four ranking repros above compile and run to exit 0, asserted in
`Test/test_cpp_interop.cb` alongside the existing `std.string` constructor coverage.
