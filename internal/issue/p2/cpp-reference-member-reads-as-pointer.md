# A C++ class member of reference type reads as a pointer (`pair<const int&, const int&>.first` is `int*`)

Reading a reference-typed data member of an imported C++ class yields the address, not the referent:
`printf("%d", p.first)` prints an address (wrong code, silently), and `int a = p.first;` is refused as
"a pointer is not a number". C++ reads the referred-to int. Found by T31 round 3 (2026-10-03), independent of
brace lists; reproduced on master 33866560.

## Repro

```cpp
// pr.hpp
#include <utility>
namespace std { namespace t31pr {
inline int g1 = 4, g2 = 5;
inline std::pair<const int&, const int&> refs() { return {g1, g2}; }
} }
```

```cflat
import cpp "pr.hpp";
extern int main()
{
    auto p = std.t31pr.refs();
    int a = p.first;                       // expected 4; actual: refused, field type is int*
    printf("pair %d %d\n", p.first, p.second);   // expected "pair 4 5"; actual prints addresses
    return a - 4;
}
```

Copy kept in scratch/pairref/ (gitignored).

## Root cause (GUESS)

Reference-typed record fields are lowered as pointer fields (the storage is a pointer) but the field access
does not add the implicit dereference that a reference member needs; the CFlat type of the member is the
pointer type.

## Fix direction

Map a C++ `T&` / `const T&` data member to an alias-like field: reads load through the stored pointer, writes
(non-const) store through it, `&obj.member` gives the referent's address - as clang does.
