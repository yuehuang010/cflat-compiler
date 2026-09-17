# A field whose type is a STANDARD-LIBRARY specialization is still embedded as opaque bytes

Filed 2026-09-16 while fixing `cpp-nested-member-class-field-embedded-opaque` /
`cpp-specialization-field-embedded-opaque`. Those covered a specialization declared by the bound
header itself; a `std::` one is deliberately left out of that fix.

## Summary

A C++ class holding a standard-library specialization by value still lays the field out as opaque
storage, so its members never resolve. Unchanged by the fix - measured identical on the pre- and
post-fix binaries of `fix/cpp-opaque-field`.

## Repro

`scratch/opaque_b01.h`:

```cpp
#pragma once
#include <vector>
namespace opqb { struct HoldsVec { std::vector<int> v; int tail; HoldsVec() : tail(5) { v.push_back(9); } }; }
```

```cflat
import cpp "opaque_b01.h";
extern int main() { opqb.HoldsVec h = default; if (h.v.size() != 1) return 131; return 0; }
```

    opaque_b01.cb(7,12): Undefined variable size.

`h.tail` (the plain field next to it) does resolve, so only the class-typed field is lost.

## Root cause

The fix emits a field's specialization as a record of its own, from the extractor, only when that
specialization is declared IN SCOPE (inside the bound header's directories). A `std::` one is owned
by the type-REQUEST path, which keys it on its own CFlat spelling (`std.vector$int`). Emitting it
from a field walk claims that identity first and was measured to break member access through an
already-requested `std.shared_ptr` field of a `[cpp]` CFlat struct
(`test_cpp_interop.cb`: `the function 'leaf' is not known`). The in-scope gate is the reason the
standard-library case was not carried.

## Fix direction

Route the out-of-scope case through the request path instead of the extractor: collect the
standard-library specializations a record holds by value alongside
`CollectCxxMemberRequestItems` and request them before `RegisterCRecords` lays the owner out, so
the one registration keyed on the request's CFlat spelling is the one the field maps to. Cost has
to be weighed - each request is a pair of Clang parses, and a header import can name many.
