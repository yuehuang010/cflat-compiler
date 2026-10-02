# C++20 std::format_to_n is unreachable from CFlat

Summary: `std::format_to_n` cannot be bound from CFlat even with a valid `char*`, 64-bit output size, format literal, and integer argument.

Repro:
```cflat
import cpp "format";
extern int main()
{
    char[16] output = default;
    auto result = std.format_to_n(&output[0], (i64)15, "value={{}}", 42);
    return result.size != 8;
}
```

Observed: compile error: `'format_to_n' is not a member of namespace 'std'` with clang reporting no matching function call.
Expected: format into the provided bounded output iterator and return the written count.
Suspected area: C++20 format-string overload binding or returned `format_to_n_result` wrapper.

Also seen in `test_libs/std/std_20_95_format_to_n.cb`.
