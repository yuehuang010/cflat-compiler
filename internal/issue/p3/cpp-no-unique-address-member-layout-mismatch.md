# C++ class with a `[[no_unique_address]]` empty member: layout refused (cflat bigger than clang)

```
C++ class 'nua.Buf' layout is not representable: cflat lays it out as 20 bytes, clang as 16
```

CFlat gives an empty `[[no_unique_address]]` member its own storage; clang (Itanium) overlaps it
with the next member, so the class is refused. Found 2026-09-26 probing fmt 12.2:
`fmt.memory_buffer` (`basic_memory_buffer` holds `FMT_NO_UNIQUE_ADDRESS Allocator alloc_;`)
is refused (544 vs 536). Standard containers and allocator-aware classes use this pattern widely.

## Repro (standalone, ~1 s)

```cpp
// nua.h
#pragma once
namespace nua {
struct Empty {};
struct Buf { char store[12]; [[no_unique_address]] Empty alloc; int n = 0; Buf() : store{}, n(4) {} };
}
```

```cflat
import cpp "nua.h";
extern int printf(const char* fmt, ...);
extern int main()
{
    nua.Buf b = default;
    printf("n=%d size=%d\n", b.n, (int)sizeof(nua.Buf));   // want n=4 size=16
    return b.n == 4 ? 0 : 1;
}
```

## Fix direction

Take field offsets from clang's `ASTRecordLayout` (`getFieldOffset`) for the extracted record, and
treat a zero-size `[[no_unique_address]]` field (`FieldDecl::isZeroSize`) as occupying no bytes -
omit it from the LLVM struct or map it onto padding, keeping field indices consistent for any
access to later members. The same check applies to a non-empty `[[no_unique_address]]` member whose
tail padding clang reuses. Look near the "layout is not representable" diagnostic (layout
verification in `LLVMBackend_CInterop.cpp` / `CClangExtract.cpp`). Header cache version bump needed
if the recorded layout changes.

## Notes for the fixing agent

- Regression: add `nua.h` to an existing `Test/library/cpp_interop_*.h` fixture, and assert both
  `sizeof` and a read of the member AFTER the empty one. No new test files.
- Do NOT add fmt to the test run (maintainer, 2026-09-26). Local check only:
  `bash scratch/probe3/run.sh` (f04).
