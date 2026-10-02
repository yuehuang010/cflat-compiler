# C++ interop cannot bind `std::tie`

## Repro

```cflat
import cpp "tuple";
extern int main()
{
    int x = 0; int y = 0;
    auto values = std.make_tuple(4, 5);
    std.tie(x, y) = values;
    return x == 4 && y == 5 ? 0 : 1;
}
```

## Observed vs expected

Observed: CFlat says `tie` is not a member of namespace `std` because its C++ free-function wrapper cannot be registered.
Expected: `std::tie` returns a tuple of references and assigns the tuple elements to the lvalues.

## Suspected area

Importing and binding free functions whose return type is a tuple of references.
