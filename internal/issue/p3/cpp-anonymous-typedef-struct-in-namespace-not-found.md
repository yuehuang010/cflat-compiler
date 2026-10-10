# Anonymous C++ `typedef struct { ... } TS;` inside a namespace cannot be named

Found by T61 review 2 (pre-existing on master).

## Repro (scratch/repro_keep/t61_rev/rev2_t61/td2.cb + w2.hpp)
```cpp
namespace w2 { typedef struct { int v; } TS; }
```
```cflat
import cpp "w2.hpp";
extern int main() { w2.TS t = default; t.v = 4; return t.v; }
```
cflat: "cannot find the type 'w2::TS'". clang++ -std=c++20: valid (the typedef name is the
struct's name for linkage purposes). The same typedef at global scope works in cflat.

## Fix direction
The anonymous-record typedef naming path (see internal/c-interop-anon-records.md) likely keys only
global-scope typedefs; extend it to namespace-scoped typedef names.
