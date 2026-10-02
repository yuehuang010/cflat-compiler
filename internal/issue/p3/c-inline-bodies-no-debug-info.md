# C/C++ header inline bodies carry no debug info under -g

Found 2026-10-01 by the V5 round-5 review (pre-existing; not caused by V5).

## Summary
With `-g`, the inline-body bitcode CFlat links in for C header inline functions (static inline,
plain inline, the `<name>.cflat_call` / `__cflat_c_inline_*` copies) has no `DISubprogram`; the
whole module carries one. Stepping into a header inline in a debugger shows no source.

## Repro
Any `.cb` importing a C header with a `static inline` function that the program calls, compiled
with `-g --out-lli x.ll`: grep the inline's `define` - no `!dbg`. Probe from the V5 review:
scratch/repro_keep/v5/rv5r3/p3.cb.

## Fix direction
Emit the inline bodies with clang's `-debug-info-kind` matching the CFlat `-g` level when the
program is compiled with `-g`, and key the c-inline bitcode cache on it (same token scheme as
`c-inline-v9`). Acceptance: `-g` IR shows `!dbg` on the inline definitions; lldb breaks inside one.
