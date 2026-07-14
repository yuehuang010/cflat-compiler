# Win32 apps ship with NO manifest, so they run classic (v5) common controls

Created: 2026-07-13 (found while dark-theming the gallery on Win32)

## Summary

A cflat-linked executable contains no application manifest at all - not just no
comctl32 v6 dependency, no `assemblyIdentity` of any kind. Every cflat GUI app
therefore binds the CLASSIC (v5) common controls from `user32`/`comctl32`, not
the themed v6 ones.

Verify on any built exe:

```powershell
$b = [System.IO.File]::ReadAllBytes("out\gallery.exe")
[System.Text.Encoding]::ASCII.GetString($b).Contains("Microsoft.Windows.Common-Controls")   # -> False
```

## Consequences

1. `SetWindowTheme` is a NO-OP on control CLIENT areas. Any fix written as
   `SetWindowTheme(ctl, "DarkMode_Explorer")` does nothing for the control's body.
   (It still works for NON-client parts, which is why the frame's scrollbar could
   be darkened.) Only color messages and owner-draw / custom-draw reach the body.
2. Push BUTTONS, the COMBO box, the TRACKBAR thumb and the status bar's size-grip
   GLYPH stay light in dark theme - they expose no color message and (see 3) no
   custom-draw hook.
3. **`tryCustomDrawButton` in `core/ui_native/win32.cb` is dead code in a live
   window.** A v5 `BUTTON` never sends `NM_CUSTOMDRAW`, so the accent-button paint
   path never runs on screen. The self-test passes only because
   `nativeTestCustomDrawFill` calls the draw function directly into a memory DC,
   bypassing the OS - i.e. the test certifies something that does not happen.
   This is a false-confidence test and should be fixed either way.

## Repro

Build any gallery/UI example, flip to the dark theme, and look: ListView, TreeView,
TabControl, ProgressBar and the status bar are dark (they were fixed via color
messages + owner-draw), but the buttons, the combo box, the trackbar thumb and the
grip glyph remain light.

## Fix direction - DECIDED 2026-07-13: embed the comctl32 v6 manifest

**Embed a comctl32 v6 manifest** in the emitted exe: `/manifest:embed` plus a
`/manifestdependency:` on `Microsoft.Windows.Common-Controls 6.0.0.0`, added to the
COFF `linkArgStrs` in `EmitExecutable` (`LLVMBackend.h` - the vector that already
carries `/out:`, `/subsystem:console`, `/DEBUG`). This is a compiler/linker change,
NOT a `win32.cb` change, and it applies to the Windows path only.

It modernizes every control at once, makes `SetWindowTheme` genuinely work on control
client areas, and lets much of the owner-draw added in the Phase-4 theming pass be
DELETED rather than grown. It also re-enables `NM_CUSTOMDRAW` for `BUTTON`, which makes
`tryCustomDrawButton` (item 3 above) real.

Accepted costs, to be handled as part of the work:
- It changes the appearance, and in places the BEHAVIOUR, of every cflat GUI app.
  Specifically, the ProgressBar and status-bar fixes work precisely BECAUSE those
  controls are unthemed (`SetWindowTheme(ctl, "", "")` + color messages); a THEMED
  control ignores those colors. Those two must be re-checked first.
- `InitCommonControls()` must become `InitCommonControlsEx` with the classes in use, or
  v6 class registration can fail and `CreateWindowExA` returns null.
- The TabControl owner-draw probably STAYS: `SysTabControl32` has no dark theme even
  under v6.

REJECTED alternative, for the record: stay on v5 and owner-draw the rest
(`BS_OWNERDRAW` buttons, `CBS_OWNERDRAWFIXED` combo, custom-painted trackbar + grip).
Contained to `win32.cb` with no blast radius, but it is more hand-painted code to own
and cflat apps would keep the classic control look.

The step-by-step is `internal/plan/ui-win32-native-polish.md` (Phase 5), which also
holds the verification requirements - headless suites cannot catch a regression here,
so it needs a visual pass over every GUI example in both themes.

UPDATE 2026-07-13: the LINKER side is verified (lld-link embeds the manifest itself, no
`mt.exe`; `/manifestinput:` works, so arbitrary manifest content and fragment merging are
available). What is still open is HOW the manifest content is declared - hardcoding the
dependency in `EmitExecutable` is rejected as baking a Windows vocabulary item into the
compiler. See Phase 5.1a of the plan for the brainstorm (declare it in `.cb` source, keep
the vocabulary in `core/`, validate via `CreateActCtxW`).
