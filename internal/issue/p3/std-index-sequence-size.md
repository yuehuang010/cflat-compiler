# std::integer_sequence size is unreachable from CFlat

## Repro

```cflat
import cpp "utility";
extern int main()
{
    std.index_sequence<0, 1, 2> values = default;
    return values.size() == 3 ? 0 : 1;
}
```

## Observed vs expected

Observed: compilation reports unknown identifier `size` for the C++ `std::integer_sequence::size()` member.
Expected: C++14 `integer_sequence` and `make_index_sequence` expose their sequence length.

## Suspected area

C++ class template member binding for `std::integer_sequence`.
