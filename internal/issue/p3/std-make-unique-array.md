# std::make_unique array form is unreachable from CFlat

## Repro

```cflat
import cpp "memory";
extern int main()
{
    auto values = std.make_unique<int[]>(2);
    return 0;
}
```

## Observed vs expected

Observed: compilation reports that `std.make_unique` has an explicit type argument that cannot be spelled in C++.
Expected: the C++14 `std::make_unique<T[]>(n)` overload is callable and yields an owning array pointer.

## Suspected area

C++ template argument spelling for array types and deduction of C++ function template results.

Also seen in `test_libs/std/std_20_92_make_shared_array.cb`: the same array template argument spelling rejection blocks `std::make_shared<T[]>`.
