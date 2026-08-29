# Resource embedding (needs a full spec before any implementation)

Ruling 2026-08-28 (maintainer): this needs a FULL SPEC, not a small-issue fix. Moved to plan
status; replaces `internal/issue/p3/no-resource-embedding-or-resource-compiler.md` (deleted).
Do not implement any piece of it - including the minimal embed primitive - until the spec
below is written and ratified.

## Motivation (from the closed issue)

Filed 2026-08-21 from the MemPressMonitor Win32 port (v0.11.0 issue 09); the reporter named
the missing `rc.exe` equivalent the LARGEST single porting cost of the exercise:

- The dialog-based main window was rebuilt as a hand-registered window class -
  re-implementing `WM_ERASEBKGND` painting and `WM_SETFONT` broadcast that the dialog manager
  gives for free (the `.rc` line `FONT 9, "Segoe UI"` became a hand-written
  `_applyControlFont()`). Both omissions were silent, caught only by eyeballing screenshots.
- The app icon shipped as a generated `u8[6960]` array plus a hand-written `ICONDIR` walker
  feeding `CreateIconFromResourceEx`, plus a build-script step to regenerate the array.

## Candidate surface recorded so far (NOT ratified)

```cflat
resource icon "app.ico";                 // RT_GROUP_ICON + RT_ICON
resource data "payload.bin" as PAYLOAD;  // RT_RCDATA, addressable by name
```

`lld-link` already emits resource sections, so the embed primitive is linker-level work, not
a dialog-template compiler. The `manifest` declaration is the precedent for one focused
declaration instead of a resource toolchain.

## What the spec must cover

- The declaration grammar and name/ID model (string names vs numeric IDs, `as NAME`).
- Which resource kinds are in scope for v1: RT_RCDATA, RT_GROUP_ICON/RT_ICON, version info?
  Dialog templates / string tables / accelerators explicitly out of scope or staged.
- Cross-platform story: PE resources are Windows-only - what does the same declaration mean
  on macOS/Linux (embedded section? synthesized byte array? error?). CFlat is cross-platform;
  a Windows-only declaration needs an explicit ruling.
- Retrieval API: FindResource/LoadResource passthrough vs a portable `resource.get("NAME")`.
- Version-info block: fixed field set and its declaration spelling.
- Build behaviour: path resolution relative to the source file, staleness/dependency
  tracking (interaction with the `<out>.cflat-dep.json` up-to-date check).
- Verification: `example.bat` gate - a `example/ui/` app embedding an icon and loading it
  back from its own module, asserting a non-null handle. Implementation and gate are
  Windows-host-bound.
