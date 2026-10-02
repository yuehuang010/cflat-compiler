# `std::unique_ptr` cannot instantiate a custom-deleter specialization

## Repro

```cflat
import cpp "memory";
int destroyed = 0;
void destroy_int(move int* p) { delete p; destroyed = destroyed + 1; }
extern int main()
{
    std.unique_ptr<int, function<void(int*)>> p = std.unique_ptr<int, function<void(int*)>>(new int(10), destroy_int);
    return *p == 10 ? 0 : 1;
}
```

## Observed vs expected

Observed: CFlat reports that `std::unique_ptr<int, void (int *)>` is an invalid specialization.
Expected: the custom-deleter specialization constructs, owns the integer, and calls its deleter at destruction.

## Suspected area

Representing a C function pointer as the deleter template argument to a C++ class template.
