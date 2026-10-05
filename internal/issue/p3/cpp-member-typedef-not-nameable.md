# Member typedefs / nested type names of a C++ class are not spellable (std.vector<int>.value_type, iterator_traits<...>.value_type)

A C++ class member type alias such as `std::vector<int>::value_type` or `std::iterator_traits<It>::value_type`
cannot be named from CFlat, neither as a local's type nor as a template argument. Workaround: `auto`, or the
element type spelled directly. Blocks trait checks like `std::is_same<traits::value_type, int>`.

## Repro

```
import cpp {"iterator", "type_traits", "vector"};
extern int main() {
    std.vector<int>.value_type x = 5;
    printf("%d\n", x);
    return 0;
}
```
Expected: prints 5. Actual: `error: the compiler expected ';' at 'x'`.

```
bool b = std.is_same<std.vector<int>.value_type, int>.value;
```
Actual: `value argument 'std.vector<int>.value_type' does not fold to an integer` (the name is parsed as an expression).

## Root-cause guess (unverified)

The grammar / ForwardRefScanner only accepts `Name<Args>` as a type, not `Name<Args>.member` for a member alias; the
bridge exposes no member typedef lookup on a class template specialization.

## Fix direction

Resolve `Spec.name` in type position against the clang record's member typedefs / nested types (and
`typename`-dependent traits such as `iterator_traits<It>.value_type`).

Found by: test_libs/std_full/std_full_11_iterator (2026-10-02); DISABLED case std_full_11_921_member_typedef.
