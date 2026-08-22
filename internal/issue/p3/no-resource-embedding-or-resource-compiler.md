# No `rc.exe` equivalent: no way to embed resources (icon, dialog template, version info)

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 09). Named by
the reporter as the LARGEST single porting cost of the whole exercise.

## Cost

The C++ application being ported had a `.rc` carrying a dialog template, an app icon, and version
info. CFlat has no counterpart, so:

- **The dialog-based main window was rebuilt as a hand-registered window class.** That meant
  re-implementing what the dialog manager does for free: `WM_ERASEBKGND` background painting, and
  `WM_SETFONT` on every child - the single `.rc` line `FONT 9, "Segoe UI"` became an
  `_applyControlFont()` that builds a DPI-scaled font and broadcasts it to the control tree. Both
  omissions were caught only by eyeballing screenshots against the C++ build, i.e. they are silent.
- **The app icon shipped as a generated `u8[6960]` array** plus a hand-written `ICONDIR` walker
  feeding `CreateIconFromResourceEx`, plus a build-script step to regenerate the array from the
  `.ico`.

## Fix direction

The reporter's own framing is the right scope: **even a minimal declaration that embeds an
arbitrary file as `RT_RCDATA` / `RT_GROUP_ICON` would remove most of this.** That is a linker-level
feature (`lld-link` already emits resource sections), not a dialog-template compiler, and it lands
independently of any decision about `.rc` parsing:

```cflat
resource icon "app.ico";                 // RT_GROUP_ICON + RT_ICON
resource data "payload.bin" as PAYLOAD;  // RT_RCDATA, addressable by name
```

Version info is the natural second step (a fixed set of string fields is easier than parsing `.rc`)
and is what makes a shipped exe look finished in Explorer.

Full `.rc` support - dialog templates, string tables, accelerators - is a much larger project and
should be judged separately, after the embed primitive exists. The `manifest` declaration is
already a precedent for "one focused declaration instead of a resource toolchain"; see
[[manifest-declaration-is-undocumented-and-unvalidated]].

## Regression test

`example.bat` is the natural gate: an `example/ui/` app that embeds an icon and loads it back from
its own module, asserting a non-null handle.
