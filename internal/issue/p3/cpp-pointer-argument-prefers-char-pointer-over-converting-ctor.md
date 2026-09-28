# An `int*` argument picks a `const char*` overload over a class it converts into

Given `fc(P)` (with `P(const int*)`) and `fc(const char*)`, `fc(ip)` with `int* ip` calls
`fc(const char*)`. C++ picks `fc(P)`: `int*` does not convert to `const char*`, so the only viable
candidate is the user-defined conversion. Pre-existing on master (master picks `const char*`
too, and before the pointer-argument fix `fc(P)` was not even offered). Found 2026-09-27
reviewing fix/ptr-arg-ctor.

## Repro

```cpp
// fc.h
#pragma once
namespace ov { struct P { int v = 0; P(const int* p) : v(*p) {} };
inline int fc(P p) { return 1; } inline int fc(const char* p) { return 2; } }
```

```cflat
import cpp "fc.h";
extern int main()
{
    int k = 7;
    int* ip = &k;
    return ov.fc(ip);   // exits 2; C++ gives 1
}
```

## Root cause guess

The ranker's empty-TypeName arm (LLVMBackend_Overloads.cpp) scores a blanked primitive pointer
against any pointer parameter through opaque `CompareUpconvert` (ptr == ptr), so `int*` is a
perfect match for `const char*`. The pointee is available in `InferSourceTypeName` for a proven
single-level pointer (`IsProvenPrimitiveSinglePointerArg`); a proven mismatched primitive pointee
should not match a primitive pointer parameter. Build the accept-set first (char-literal rules).
