# CFlat cannot derive from `std::enable_shared_from_this`

## Repro

```cflat
import cpp "memory";
struct SelfShared : std.enable_shared_from_this<SelfShared>
{
    int value = default;
};
extern int main()
{
    std.shared_ptr<SelfShared> owner = std.make_shared<SelfShared>();
    owner->value = 17;
    std.shared_ptr<SelfShared> again = owner->shared_from_this();
    return again->value == 17 && owner.use_count() == 2 ? 0 : 1;
}
```

## Observed vs expected

Observed: the imported `std.enable_shared_from_this` is not recognized as a C++ class base.
Expected: a CFlat-derived class can use `shared_from_this` after construction through `make_shared`.

## Suspected area

Importing standard class templates for use as C++ base classes.
