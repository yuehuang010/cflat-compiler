# C variadic function with a C++ inline body fails to link

Found by T58 review 4 (pre-existing, master same).

## Repro (scratch/repro_keep/t58_rev/rev4/)
```cpp
#include <cstdarg>
inline int vsum(int n, ...) { va_list ap; va_start(ap, n); int s = 0;
    for (int i = 0; i < n; ++i) s += va_arg(ap, int); va_end(ap); return s; }
```
```cflat
import cpp "v.hpp";
extern int main() { return vv.vsum(2, 3, 4); }   // link error: undefined symbol for vsum
```
clang++ -std=c++20: links (inline function emitted in the TU that odr-uses it).

## Fix direction
The on-demand emission of inline C++ bodies (R4 member bodies on call ruling) likely skips
variadic functions because no wrapper is generated for them; emit the inline definition (or a
non-variadic forwarding is impossible - so emit the body itself) when a CFlat call references it.
