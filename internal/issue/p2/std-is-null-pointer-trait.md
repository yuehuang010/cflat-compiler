# std::is_null_pointer reports false for std::nullptr_t

## Repro

```cflat
import cpp "type_traits";
extern int main()
{
    return std.is_null_pointer<std.nullptr_t>.value ? 0 : 1;
}
```

## Observed vs expected

Observed: the expression evaluates false at runtime.
Expected: `std::is_null_pointer<std::nullptr_t>::value` is true.

## Suspected area

C++ type translation or static member binding for `std::nullptr_t` and type traits.
