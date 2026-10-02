# CFlat lambda cannot initialize `std::function`

## Repro

```cflat
import cpp "functional";
extern int main()
{
    std.function<int(int)> f = (int x) => { return x + 3; };
    return f(4) == 7 ? 0 : 1;
}
```

## Observed vs expected

Observed: CFlat cannot infer the lambda return type because no `function<...>` or `Lambda<...>` type reaches the lambda body.
Expected: the target `std::function<int(int)>` supplies the signature and invokes the CFlat lambda.

## Suspected area

Propagating the target callable signature from an imported C++ class template initializer into a CFlat lambda.

Also blocks `std.visit` with a lambda visitor (`std.visit` itself works with `std.function<int(int)>(free_fn)`, enabled in test_libs/std/std_17_vocabulary.cb).
