# C interop: function-like macros are not imported (and the object-like/function-like asymmetry is undocumented)

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 08).
Triaged on `cd847a3` - **only the macro bullet survives**; the two record-mapping bullets in the
original report are NOT reproducible on the current tree (measured, below).

## Confirmed: function-like macros are skipped

`doc/C_INTEROP.md:113` states the rule: object-like `#define` constants ARE folded and surfaced as
bare globals; a macro that does not fold to a constant is skipped. Function-like macros therefore
never come through, so every one of these has to be hand-transcribed:

- the `ListView_*` family - every ListView call had to be written as a raw
  `SendMessageW(list, LVM_..., ...)`
- `MAKEINTRESOURCE`
- the `GetPerformanceInfo` -> `K32GetPerformanceInfo` alias

The asymmetry is what makes it costly: object-like constants fold, so the user reasonably expects
macros to work, and discovers per-macro that some do not. Two independently useful fixes:

1. **Document the asymmetry prominently** in `doc/C_INTEROP.md` - "function-like macros are not
   imported; write the underlying call" with the `ListView_*` case as the worked example. Cheap,
   and removes the surprise.
2. **Import the simple ones.** A large fraction of the Win32 function-like macros are a single
   expression over their parameters with no token pasting and no statement bodies - exactly what
   clang can expand mechanically. A conservative rule (single expression, no `##`, no varargs, all
   parameters used as values) would cover `ListView_*` and `MAKEINTRESOURCE`, and anything outside
   the rule stays skipped, as today. Related: a simple ALIAS macro
   (`#define A B`, where `B` is a known function) should register as an overload of the target.

Related papercut from the same report: `MAKEINTRESOURCE` constants fold to ints, so a
pointer-typed parameter needs a double cast - `LoadCursorW(nullptr, (u16*)(i64)IDC_ARROW)`. If (2)
lands, the macro itself carries the pointer type and the double cast disappears.

## NOT reproducible - do not re-file without a fresh repro

Both were re-measured on `cd847a3`, Release, and both pass. They were presumably fixed between
v0.11.0 and now (see `internal/c-interop-anon-records.md`, the 2026-06 anonymous-record work).

| Reported | Measured now |
|----------|--------------|
| Unions in imported structs: `PDH_FMT_COUNTERVALUE_ITEM_W` had to be re-declared by hand with the union pinned to its `LARGE` arm | `import {"windows.h","pdh.h"} lib "pdh.lib";` then `item.FmtValue.largeValue` type-checks clean |
| Anonymous/unnamed nested structs: `SYSTEM_PROCESS_INFORMATION` had to be re-declared field-by-field with explicit padding | `import {"windows.h","winternl.h"};` then `sizeof(SYSTEM_PROCESS_INFORMATION)` prints **256** - the correct layout, computed by the compiler |

Repro files for both are in `scratch/triage/` at filing time (scratch is gitignored; re-create from
the table above if needed).

## Regression test

`Test/test_windows.cb` already covers the record mapper. If (2) is implemented, add a
`ListView_*`-shaped function-like macro leg there.
