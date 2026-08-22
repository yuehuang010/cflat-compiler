# `--subsystem windows` landed; the Windows-host verification legs are still outstanding

Filed 2026-08-21 (MemPressMonitor Win32 port, v0.11.0). The hardcoded `/subsystem:console` was
FIXED in the p2 bundle. What remains is verification that could not be run from a macOS host.

## What landed

- `cflat/main.cpp`: `--subsystem <console|windows>` CLI option; a value other than those two is
  rejected with `Error: --subsystem must be 'console' or 'windows' (got '<x>').` and exit 1.
- `cflat/LLVMBackend.h` / `LLVMBackend_StateAndImports.cpp`: `windowsSubsystem_` (default
  `"console"`) + `SetWindowsSubsystem`.
- `cflat/LLVMBackend_EmitAndLink.cpp`: passes `"/subsystem:" + windowsSubsystem_`, and for
  `windows` also passes `/entry:mainCRTStartup` - `/subsystem:windows` otherwise makes `lld-link`
  look for `WinMain`, which no CFlat program has.
- `doc/CLI.md` documents the flag.

Verified on macOS only to the extent the host allows: the flag parses, the bad-value path errors
with exit 1, and the good-value path reaches the linker (the Mach-O link then ignores it).

## Residual - needs a Windows host

1. Build any `example/ui_native/*.cb` with `--subsystem windows -o app.exe` and assert the PE
   header's `Subsystem` field is `2` (`IMAGE_SUBSYSTEM_WINDOWS_GUI`), e.g. via `dumpbin /headers`
   or a few bytes read at the optional-header offset. Assert the default build still reports `3`.
2. Assert the launched GUI app has no console window - the `EnumWindows` top-level-window
   inventory described below, since by-handle screenshot capture cannot see the stray console.
3. Decide whether `core/ui_test.cb` should gain that inventory permanently.

## Why (2) matters (from the original report)

The reporter's GUI port carried the stray console for the entire port without anyone noticing:
their screenshot-parity testing grabbed the app window by handle, so the console sitting next to it
never appeared in a capture. It was caught only when they added an `EnumWindows` inventory
asserting that their window class was the only visible top-level window of their pid. The shipped
`core/ui_test.cb` captures by handle too, so it cannot catch this today.
