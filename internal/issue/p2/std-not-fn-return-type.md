# std::not_fn return type is unsupported by C++ interop

## Repro

```cflat
import cpp "functional";
bool is_zero(int x) { return x == 0; }
extern int main()
{
    std.function<bool(int)> zeroFn = std.function<bool(int)>(is_zero);
    auto pred = std.not_fn(zeroFn);
    return pred(1) ? 0 : 1;
}
```

## Observed vs expected

Observed: CFlat cannot bind the C++ free function because `std::not_fn` returns the implementation type `std::__not_fn_t<std::function<bool(int)>>`.
Expected: the returned predicate adapter can be invoked from CFlat.

## Suspected area

Support for standard-library callable adapter return types.
