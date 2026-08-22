# `/subsystem:console` is hardcoded: every GUI app ships with a stray console window

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 - surfaced in their
README's verification notes, not in their issue list). Confirmed on `cd847a3` by reading the tree.

## Confirmed

`cflat/LLVMBackend_EmitAndLink.cpp:1202` passes `"/subsystem:console"` to `lld-link` as a fixed
argument. There is no CLI flag (`doc/CLI.md` lists `-o`, `--out-lli`, `--out-asm`, `--bitcode` and
nothing about the subsystem), no source declaration, and nothing in `cflat/core/` calls
`FreeConsole` / `ShowWindow(GetConsoleWindow(), SW_HIDE)` to paper over it - grep across `core/` and
`example/` returns nothing. So **every** Windows GUI program built with CFlat, including the shipped
`ui_native` examples, gets a console window it never asked for.

## How it was found, and why that matters

The reporter's GUI port carried the stray console **for the entire port** without anyone noticing:
their screenshot-parity testing grabbed the app window by handle, so the console sitting next to it
never appeared in a capture. It was caught only when they added an `EnumWindows` inventory
asserting that their window class was the only visible top-level window of their pid.

That is the interesting part of this issue. It is not subtle to a user - it is the first thing
anyone sees on launch - but it is invisible to by-handle screenshot testing, which is exactly what
`core/ui_test.cb` does. So the shipped UI test framework cannot currently catch it either.

## Fix direction

1. **A way to select the subsystem.** A CLI flag (`--subsystem console|windows`) is the minimum. A
   source-level declaration is the better fit for CFlat's style and matches how `manifest` already
   works - the choice is a property of the program, not of the invocation, and a GUI app should not
   depend on the caller passing a flag.
2. **Infer it where possible.** A program whose entry point is a windowed host (`ui_native`,
   `startHeadlessWindow`, a registered `WNDCLASS` message loop) wants `windows`; the mainstream case
   stays `console`. Inference alone is not enough - keep the explicit spelling for direct Win32 apps
   that never touch `ui_native`.
3. Note the entry-point consequence: `/subsystem:windows` looks for `WinMain` by default. Either
   pass `/entry:` explicitly so `main` keeps working, or emit a thunk. Do not make users write
   `WinMain`.

Related, from the same reporter: [[no-resource-embedding-or-resource-compiler]] - both are "the
`.rc` / linker-configuration surface that CFlat has no counterpart for", and both were absorbed by
hand-written code in their `gui.cb`.

## Regression test

Two legs, and the second is the one that would actually have caught this:

- `example.bat`: build a `ui_native` example and assert the PE subsystem field in the produced exe
  is `2` (GUI), not `3` (CUI).
- `core/ui_test.cb`: add a top-level-window inventory check to the standard hardening kit -
  enumerate visible top-level windows of the test's own pid and assert the app window is the only
  one. That closes the class of defect that by-handle screenshotting is blind to.
