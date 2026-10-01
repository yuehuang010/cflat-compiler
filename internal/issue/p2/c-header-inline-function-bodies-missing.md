# C header inline functions: static inline never bound, plain inline has no body under `import`

Found 2026-09-30 from the windows.h tier-1 smoke test: `HRESULT_FROM_WIN32` (winerror.h,
`FORCEINLINE`) links under `import cpp "windows.h"` but fails under a C `import "windows.h"`.
Probing a plain header shows the gap is general, not Windows-specific, and wider for `static inline`.

## Repro (macOS, master 0e1dbfa0)
```c
// inl.h
static inline int add3(int x) { return x + 3; }
inline int add4(int x) { return x + 4; }
#define ADD5(x) add3(x) + 2
```
```cflat
import "inl.h";            // or: import cpp "inl.h";
int main() { return add3(1) == 4 ? 0 : 1; }   // swap in add4(1) == 5, ADD5(1) == 6
```

| Call | `import` (C) | `import cpp` |
|------|--------------|--------------|
| `add3` (static inline) | "Undefined variable add3." | "Undefined variable add3." |
| `add4` (inline) | compiles, link fails: undefined symbol `_add4` | works |
| `ADD5` (macro over static inline) | "Undefined variable ADD5." | "Undefined variable ADD5." |

## Root cause (hypothesis, verify first)
- C path binds prototypes only; a C99 `inline` definition emits no external symbol, so the binding
  points at a symbol nobody defines. The C++ path's demand-body machinery (CxxIncrementalGroup /
  CheckCxxDemand) compiles ODR-used bodies, which is why `import cpp` works for plain inline.
- `static inline` (internal linkage) appears to be filtered at harvest on BOTH paths, so the name is
  never registered; the function-like macro that calls it then drops too.

## Fix direction
- Route used inline / static inline C functions through the same on-demand body path as C++ member
  bodies (demand-only, ruling R4 style: signature eager, body emitted only when called), with the
  body emitted as a local (internal / linkonce) definition in the CFlat module.
- Register static inline functions at harvest instead of dropping them; the macro translation then
  resolves as with any other function.
- Any body that fails to compile -> one LogError at the CFlat use site, "clang: " prefix.
- Coverage: extend Test/test_c_interop.cb with the three shapes above (in-repo header, e.g.
  Test/library/c_macro_helpers.h), plus the `import cpp` column in Test/test_cpp_interop.cb.
- Severity p2: `static inline` helpers are pervasive in C headers (libc, SDL, stb-style, Win32),
  and today they vanish silently until the call site.

Attempt (W3b, 2026-09-30, no code change): the demand-body path is tied to C++ request groups
(CxxIncrementalGroup); plain C `import` has no use-site demand path, and enabling CodeGen for C
imports wholesale would emit every inline (violates demand-only). Needs a design: route called
C inline/static-inline functions through a demand request like C++ members (or a per-call clang
body request). Report + pre-fix matrix: scratch/repro_keep/w3b/.

## Ruling (maintainer, 2026-09-30)
Follow clang: a C `inline` / `static inline` function the CFlat program calls gets its body compiled
by clang (C mode, the import's own flags/defines) and emitted into the CFlat module as a local
definition, exactly as clang emits an inline body into the TU that uses it. Demand-only: uncalled
inline bodies are never compiled. This is the "C-side demand request" option above; build it on the
existing clang request machinery rather than a separate C-only harvest.
