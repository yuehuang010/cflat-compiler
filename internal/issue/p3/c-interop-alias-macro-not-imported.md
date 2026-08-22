# C interop: a bare ALIAS macro (`#define A B`, B a known function) is not imported

Residual of the retired `c-interop-function-like-macros-not-imported` issue. The
function-like-macro half of that issue was **already implemented** before it was filed against
v0.11.0 and has now been measured on the current tree (macOS, Release) - see the table below.
Only this one bullet survives.

## Confirmed gap

```c
int probe_add(int a, int b);
#define PROBE_ALIAS probe_add
```

`PROBE_ALIAS` is **object-like** (no parameter list), so it goes to the constant-folding probe
path, where its body does not fold to an integer / float / string / pointer constant and it is
dropped. It is not function-like, so `RegisterCFunctionMacros` never sees it either. It falls
between the two mechanisms and the name simply does not exist in CFlat. The real-world case
from the original report is `#define GetPerformanceInfo K32GetPerformanceInfo`.

## What already works (measured, do not re-file)

Probe header `scratch/p/inc/probe.h` at fix time; `-v` reports
`function-like C macros: 3 translated, 1 rejected, 0 skipped`.

| macro | result |
|-------|--------|
| `PROBE_BASE 100` | folded, registered |
| `PROBE_DOUBLE(x) ((x)*2)` | translated; `PROBE_DOUBLE(21) == 42` |
| `PROBE_SUM(a,b) ((a)+(b)+PROBE_BASE)` | translated; `== 103` |
| `PROBE_CALL(a) probe_add((a), PROBE_BASE)` | translated |
| `PROBE_PASTE(a,b) a ## b` | rejected by design (token paste) |
| `PROBE_ALIAS probe_add` | **not imported - this issue** |

The mechanism is the in-process clang `MacroCollector` PPCallback in `CClangExtract.cpp`
(function-like bodies are harvested into `ExtractResult::funcMacros`), and
`LLVMBackend_CInterop.cpp` `RegisterCFunctionMacros`, which emits
`auto NAME<T0, T1>(T0 a, T1 b) { return (body); }` per accepted macro. The asymmetry and the
exact reject rules are now documented in `doc/C_INTEROP.md`, section
"Macros: object-like and function-like", including this gap.

## Fix direction

In `ClassifyRawMacro` (`LLVMBackend_CInterop.cpp`), before giving up on an object-like macro
whose body did not fold: if the body is a **single identifier** that is already in
`functionTable`, register the macro name as an **overload alias** of that function rather than
dropping it. Guard on the name not already being defined, exactly as `RegisterCFunctionMacros`
does. A body that is a single identifier naming a registered GLOBAL/constant is the same shape
and could alias likewise.

## Regression test

`Test/test_c.cb` / the `--c-include` probe-header fixtures; add an alias macro to the fixture
header and call it by the alias name.
