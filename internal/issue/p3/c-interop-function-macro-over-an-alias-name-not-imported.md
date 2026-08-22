# C interop: a function-like macro whose body calls an ALIAS name is still not imported

Filed 2026-08-22 from the review of the alias-macro fix (`9580ee9`). `RegisterCFunctionMacros`
runs BEFORE `RegisterCMacroAliases` (LLVMBackend_CInterop.cpp), so a function-like macro whose
expansion names an alias is rejected because the alias does not exist yet.

## Repro (header)

```c
int rev_h2(int x);
#define REV_A rev_h2
#define REV_CALLA(x) REV_A(x)
```

`REV_CALLA(3)` -> `Undefined variable REV_CALLA.` (identical before and after 9580ee9).

This is exactly the `commctrl.h` shape: `#define SNDMSG SendMessage` with the whole
`ListView_*` / `TreeView_*` family written over `SNDMSG`, so the Windows common-controls macro
API stays unusable from CFlat.

## Fix direction

Either run a second `RegisterCFunctionMacros` pass after the alias pass (only for macros that
failed the first pass), or reorder so aliases bind first and then function-like macros are
templated against the now-complete table. Legs: extend `Test/library/c_macro_helpers.h` +
`Test/test_c_interop.cb` Section U with the `SNDMSG` shape; no new test files.

## Related, same review (record, not separately filed)

- Across SEPARATE import statements the "never alias over an existing name" guard is
  order-dependent: alias-bearing header first -> the alias binds; a later header's real
  declaration of the same name is push_back'ed as a duplicate-signature overload. Today both
  orders still resolve to the real function (probed), so benign, but any change to
  overload tie-breaking would flip it silently. Windows shape: `#define GetObject GetObjectW`
  in one header, a real `GetObject` declared by another library. Fix: when a real declaration
  arrives for a name currently bound by an alias, REPLACE the alias entry instead of appending.
- `FindFunctionSourceName(fn)` can now return the alias name in diagnostics (two keys share
  one `llvm::Function`; `unordered_map` order). Cosmetic.
- Struct-tag and type aliases are not registered with the symbol sink (`--symbol` / LSP).
