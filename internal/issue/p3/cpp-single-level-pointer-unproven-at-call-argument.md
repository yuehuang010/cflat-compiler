# Some single-level pointers are not offered to a class parameter's converting constructor

`deep(P)` with `P(const int*)` accepts `&k`, `int*` / `const int*` locals and `&a[i]`, but these
single-level pointers are refused ("no overload of 'deep' matches") while the spelled
`P(x)` works:

- `auto p = &k;` then `deep(p)`
- `deep(&*ip)` with `int* ip`
- `deep(td.mk(&k))` where the C++ function returns `int*` through `typedef int* IP;`

Found 2026-09-27 reviewing fix/ptr-arg-ctor.

## Repro

```cpp
// td.h
#pragma once
namespace td { typedef int* IP; inline IP mk(int* p) { return p; }
struct P { int v = 0; P(const int* p) : v(*p) {} }; inline int deep(P p) { return p.v; } }
```

```cflat
import cpp "td.h";
extern int main()
{
    int k = 7;
    auto p = &k;
    int* ip = &k;
    int a = td.deep(p);            // refused
    int b = td.deep(&*ip);         // refused
    int c = td.deep(td.mk(&k));    // refused
    return a + b + c == 21 ? 0 : 1;
}
```

## Root cause guess

`IsProvenPrimitiveSinglePointerArg` (LLVMBackend_CInterop.cpp) accepts only a recorded depth 1
or an unrecorded address of a slot holding a non-pointer scalar. `auto` inference, `&*`, and a
C++ return type (typedef'd or not) leave `PointerDepth` unrecorded and the value is a load or a
call, so there is no proof; none is refused wrongly by the gate, the depth is just never
recorded. Record depth 1 where these values are produced (auto from `&scalar`, `&*p` = p's
depth, C++ return types from the clang signature) rather than widen the gate.
