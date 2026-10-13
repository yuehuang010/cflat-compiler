# Fixed char array explicit cast to a C++ class is refused

PRE-EXISTING: reproduced on master and fix/t75-strcast during T75 review.

## Repro

```cpp
// array_cast.hpp
namespace ac { struct Box {
    int tag;
    Box(char*) : tag(1) {}
    ~Box() {}
    int id() const { return tag; }
}; }
```

```cflat
import cpp "array_cast.hpp";
extern int main() {
    char[4] p = {97, 98, 99, 0};
    ac.Box v = (ac.Box)p;
    return v.id() == 1 ? 0 : 1;
}
```

Both compilers refuse with "cannot store a pointer value into struct storage".
The C++20 equivalent selects Box(char*) and returns tag 1.

Root cause: explicit casts lack a fixed-array-to-constructor conversion route.
T75's new branch requires TypeAndValue.Pointer, which fixed arrays do not satisfy.
Fix direction: preserve fixed-array source metadata and apply constructor argument
array-to-pointer decay before selecting and materializing an explicit constructor.
Review evidence: scratch/rev_t75_rank_array.cb and scratch/rev_t75_more_results.log.
