# `import cpp` under a cross-target `-p linux` / `-p windows` segfaults (exit 139)

Found 2026-09-23 on macOS arm64 while reviewing fix/longdouble-native-alias (the reviewer's
`-p linux` long-double header probe). Pre-existing: master 0251af0e crashes identically, so it is
not a long-double regression. Any C++ header import is enough; no long double needed.

## Repro

scratch/ldp/min.hpp:
```cpp
#pragma once
inline int seven() { return 7; }
```
scratch/ldp/min.cb:
```cflat
import cpp "min.hpp";
int main() { return seven() == 7 ? 0 : 1; }
```
```
x64/Release/cflat scratch/ldp/min.cb -i scratch/ldp --check              # exit 0
x64/Release/cflat scratch/ldp/min.cb -i scratch/ldp --check -p linux     # exit 139
x64/Release/cflat scratch/ldp/min.cb -i scratch/ldp --check -p windows   # exit 139
```
Release lldb: EXC_BAD_ACCESS with pc in unmapped memory (0x109214000, no symbol) - an indirect
call through a bad function pointer, not a null deref. Debug: EXC_BAD_INSTRUCTION at a similar
unmapped pc. Backtrace is unusable in both because the frame chain is gone.

## Suspects

- The incremental interpreter / clang harvest is created with a target triple from `-p` while
  the host-built runtime pieces (bundled ld64.lld / libSystem stubs, or the JIT used by
  CxxIncrementalGroup for constant evaluation) stay host-targeted; a cross-target JIT or
  Interpreter would explain a jump into garbage. Start at `CxxIncrementalGroup::Create` and
  where the `-p` triple reaches the clang invocation args.
- Alternatively an ORC/JIT entry point invoked for a non-host triple.

## Fix direction

Root-cause with a Debug build and a symbolized backtrace (`thread backtrace -u`, `register read
lr`). If cross-target C++ import is genuinely unsupported (no cross-target JIT), refuse it with
a `LogError` naming the target and the import line instead of crashing. The refusal must fire
in both cold and warm cache paths.

## Acceptance

- Both `-p` runs above exit 1 with a diagnostic (or succeed), never 139.
- An `expect_error` leg in an existing err test if the refusal route is taken.
