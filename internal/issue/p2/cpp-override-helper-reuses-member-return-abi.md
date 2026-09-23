# C++ override helper reuses the base virtual member's return ABI

Found 2026-09-22 while fixing the bridge "Cannot have multiple 'sret' parameters" failure
(pull 9b825a07).

## Summary

A `[cpp] struct` that overrides a C++ base virtual routes each override through a generated free
`extern "C"` helper `__cflat_ovr_<Short>_<method>_<n>(Self*, ...)` (declared in
`MainListener_Aggregates.cpp` ~1002 as `raw.retType + " " + StableName + "(Self*, ...)"`).
`EmitCppStructOverrideThunk` (`LLVMBackend_CInterop.cpp` ~12599) builds that helper's ABI recipe
from the BASE MEMBER's `candidate.raw.abi`. The only adjustment is clearing
`ret.sretAfterThis` on a copy, which was done in the 9b825a07 fix.

The MSVC x64 ABI differs between the two function kinds:
- an instance method returns ANY class type indirectly (sret, after `this`);
- a free function returns a small trivially-copyable struct (<= 8 bytes, e.g. `{int a; int b;}`)
  in RAX.

So a base virtual that returns a small POD by value makes the helper's recipe sret while the
helper clang compiles returns in RAX. The C++ thunk then calls the helper with a hidden pointer the
helper never writes. The result is garbage or a crash, with no diagnostic. No test covers it: every
current override test returns void, a scalar, or a non-trivial class.

## Repro (untested sketch)

```cpp
// base.h
struct Small { int a; int b; };
struct Base { virtual ~Base() = default; virtual Small make() { return {1, 2}; } };
inline int call_make(Base* b) { Small s = b->make(); return s.a * 10 + s.b; }
```

```cflat
import cpp "base.h";
[cpp] struct Derived : Base { Small make() { Small s = default; s.a = 3; s.b = 4; return s; } };
extern int main() { Derived d = default; return call_make(&d); }   // expect 34
```

```
x64\Release\cflat.exe repro.cb -i . -o repro.exe && repro.exe
```

## Root cause

The helper's return classification is inherited from a member function, not computed for the free
function it actually is.

## Fix direction

Let clang own the ABI (see feedback memory "let clang own C++ ABI logic"). Classify the helper
declaration itself through the same clang ABI plan used for free functions, e.g. by extracting the
synthesized helper prototype from the request TU, instead of patching `candidate.raw.abi`
field by field. Add the repro above to `Test/test_cpp_interop_bridge.cb` (or the existing
override test file) with a small-POD return and a > 8-byte POD return.

Suggested bucket: **p2** (silent wrong-code on a plausible C++ interop pattern, no crash guard).
