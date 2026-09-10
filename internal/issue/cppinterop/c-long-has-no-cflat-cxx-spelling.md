# C++ `long` and `unsigned long` have no CFlat spelling; `long`/`ulong` spell as `long long`

Found 2026-09-10 while ruling on multi-word primitives (see the ruling section at the bottom of
`internal/fix-issue-lessons.md`). Measured on `x64/Release/cflat.exe` at `d71d670`.

This is a silent wrong answer, not a diagnostic gap. It is the first slice of
[`cxx-primitive-typing.md`](cxx-primitive-typing.md) and lands before it: that issue's identity
table has `long` and `ulong` naming C++ `long` and `unsigned long`.

## Repro

```cflat
import cpp "vector";
extern int main() { std.vector<long> v = default; v.push_back(3); return 0; }
```

    cflat p.cb --out-lli p.ll
    ?push_back@?$vector@_JV?$allocator@_J@std@@@std@@QEAAX$$QEA_J@Z

`_J` is `long long`. CFlat `long` is `longBits_` wide - 32 bits on Windows - so a 4-byte CFlat type
named an 8-byte C++ specialization. `std.vector<ulong>` lands on `_K` (`unsigned long long`) the
same way. Consequence: C++ `long` (`J`) and `unsigned long` (`K`) are unreachable from CFlat.
`std::vector<DWORD>` on Windows and `std::vector<size_t>` on LP64 are exactly those types.

## Root cause

Outbound, `LLVMBackend::CxxSpellingForCflatType` (`cflat/LLVMBackend_CInterop.cpp:3195-3196`):

    { "long", "long long" }, { "i64", "long long" },
    { "ulong", "unsigned long long" }, { "u64", "unsigned long long" },

Two CFlat types map onto one C++ type, and the wrong one of the pair is target-dependent.

Inbound compounds it. `cflat/LLVMBackend_CInterop.cpp:1174-1176` lands C `long`/`unsigned long` on
`i32`/`u32` (Windows) or `i64`/`u64` (LP64) - never on `long`/`ulong`. So a member returning
`unsigned long` reads back as `u32`, and feeding that value's type into a template argument spells
`unsigned int`: a different specialization than the one it came from. The round trip is lossy in
both directions.

## Fix direction

Outbound: `{ "long", "long" }, { "ulong", "unsigned long" }`. That is what the types ARE - CFlat
`long` is documented as C's target-native long, and it matches C++ `long` on both LLP64 and LP64.
`i64`/`u64` keep `long long`/`unsigned long long`, so both C++ types stay reachable and distinct.

Inbound: route `long`/`long int`/`signed long` to `long` and `unsigned long`/`unsigned long int` to
`ulong`, so the round trip closes. Check what this moves first - the current `i32`/`u32` landing is
load-bearing for plain C interop, where a C `long` parameter really is width-compatible with `i32`
(`DWORD*` binds as `u32*`). Follow the "C boundary identity" ruling in
[`cxx-primitive-typing.md`](cxx-primitive-typing.md); its recommendation keeps C headers
width-mapped and changes only the C++ direction.

Check while there: `LLVMBackend_CInterop.cpp:4038` and `:4333` (`one = targetIsCxxRecord ? "int" :
"long";`) both mint a bare `"long"` into a spelling path and inherit whatever this row says.

## Acceptance

- `std.vector<long>` instantiates `?$vector@J...` and `std.vector<ulong>` `?$vector@K...` on
  Windows; `std.vector<i64>` still `_J` and `std.vector<u64>` still `_K`.
- A C++ member returning `unsigned long` reads back as `ulong`, and re-spelling that type as a
  template argument reaches the same specialization it came from.
- `sizeof` agrees across the boundary on both ABIs: a `std.vector<long>` element is
  `sizeof(long)` bytes.
- Legs join the multi-word coverage in `Test/test_cpp_interop.cb` (section M21 neighbourhood),
  no new test file. Existing legs 769 and 793 assert the `i64`-vs-`long` distinction already -
  they must stay green unchanged.
