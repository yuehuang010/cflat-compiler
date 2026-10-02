# std::byte cannot be named through CFlat interop

## Repro

```cflat
import cpp "cstddef";
extern int main()
{
    std.byte value = std.byte(42);
    return std.to_integer<int>(value) == 42 ? 0 : 1;
}
```

## Observed vs expected

Observed: `std.byte` is not registered as a member of namespace `std`.
Expected: CFlat can construct a `std::byte` and pass it to `std::to_integer`.

## Suspected area

Import and binding of scoped enum types declared in standard headers.
