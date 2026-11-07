# Function-like zero macros refused as null pointer constants at C++ call sites

Summary: the NULL / nullptr / `((void*)0)` / cast macro spellings, `nullptr` into a by-value
`std::function`, pointer-ref ctors, `D(0)`, `K + 0`, `if const (Z0 == 0)` and explicit `tdp<int>(0)`
all match clang (T52, T103, T296, NULLCTOR 2026-11-05). What is left, each accepted by clang++ -std=c++20:

- Function-like `ID(0)` / `ID(ZERO)` into `lp(int*)`: "no overload" after macro-function
  evaluation (clang 5).
- Zero-arg `MZ()` into `lp(int*)`: `MZ` is undefined in CFlat (clang 5).
- `K(PID(ZERO))` with only `K(int*)`: the argument arrives as `i64` (clang 5).

Fix direction: give a function-like macro whose expansion is an integer literal zero the same
CMacroEntry.ilz identity as an object-like zero macro (identity, never spelling).
Probes: scratch/repro_keep/t52_rev/rev7_t52_p/; NULLCTOR matrix rows in the fix/nullctor report.

## Known divergences (pre-existing, not acceptance gaps)

- Const-pointee cast macros (`((const int*)0)`) stay refused at `int*`; clang also rejects (typed pointer).
- `o + 0` with `operator+(Op, int*)` and `operator+(Op, long)`: clang ambiguous, CFlat picks int* (40).
  Probes: scratch/repro_keep/t68_rev/.

RULED 2026-10-31 (maintainer), LLP64: ALIAS EVERYWHERE. CFlat's identity rule holds at C++ parameters and
template arguments: on LLP64 `long` is `int`, so `((int*)0)` / an `int*` into a C++ `long*` parameter is
ACCEPTED and deduction may give `int` where clang-cl gives `long`. Divergence from clang-cl accepted by
design; the T296 leg stays on `long long*` (distinct on both ABIs).
