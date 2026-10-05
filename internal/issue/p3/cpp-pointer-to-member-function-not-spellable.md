# `&std.string.size` (pointer to an imported C++ member function) cannot be written

`std::mem_fn(&std::string::size)` / `std::invoke(&std::string::size, s)` need a pointer-to-member. CFlat reports
`'size' is not a member of namespace 'std.string'.` (the `&Class.member` form is read as a namespace lookup).

## Repro

```cflat
import cpp {"string", "functional"};
extern int main()
{
    auto msize = std.mem_fn(&std.string.size);
    std.string s = std.string("hello");
    return msize(s) == 5 ? 0 : 1;      // expected 0
}
```

Case: test_libs/std_full/std_full_11_920_mem_fn.cb. Lambdas / `std.function` cover the practical need.

## Root cause (GUESS)

No expression form for member pointers of imported classes (`&C::m` has no CFlat spelling, and a bound
member pointer has no type); the parser/semantic pass tries a namespace path.

## Fix direction

Either add `&Class.member` as a typed member-pointer value for imported C++ classes (data and function members;
overloaded members need disambiguation), or document that member pointers are out of scope and make the
diagnostic say so.

Found by: test_libs/std_full/std_full_11_functional (2026-10-02).
