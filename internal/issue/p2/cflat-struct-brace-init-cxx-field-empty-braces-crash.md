# Designated brace init `{ v = {} }` of a CFlat struct with a C++ class field crashes the compiler

From T134 review 1 (2026-10-12), pre-existing on master c838c996e (SIGSEGV, rc 139, internal compiler error block).

```cpp
// r.hpp
#include <initializer_list>
struct DL { int n; DL() : n(-1) {} DL(std::initializer_list<int> v) : n((int)v.size()) {} ~DL() {} };
```
```cflat
import cpp "r.hpp";
struct H { r.DL v; int z; };
extern int main() { H h = { v = {}, z = 2 }; return h.v.n + h.z; }
```

clang++ -std=c++20 (`H h = { .v = {}, .z = 2 };`): value-initializes v through the default constructor, returns 1.
The reviewer saw the crash for a class with a default constructor too, so it is not specific to list-only classes.

Fix direction: the brace-init field path for a designated `field = {}` with a C++ class field type reaches
construction without a target slot or constructor; route it like a field default `= {}` (T134 added that path for
initializer_list-only classes). At minimum replace the crash with a LogError.
