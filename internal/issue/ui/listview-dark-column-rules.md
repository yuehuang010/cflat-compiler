# Dark ListView paints Explorer column rules over the body (Win11)

## Summary

On Win11, a dark-themed `ui_native` ListView (report view) shows a faint 2px
light/dark vertical rule pair at every column boundary, running the full client
height of the body. Visible in `example/ui/11-mempress/mempress.cb`; minimal
repro `scratch/lvprobe.cb`. Cosmetic only.

The tree currently carries two attempted mitigations that DO NOT work and
should be removed/reverted as part of the fix (see below):
`DarkMode_ItemsView` in `_themeListView`, and the
`WM_SHOWWINDOW` -> `WM_APP_RETHEME` re-theme machinery in `NativeWndProc`.

## Root cause (fully diagnosed 2026-08-24, opus investigation)

The rules are drawn by comctl32 as part of the Explorer-family LISTVIEW theme
(`Explorer`, `ItemsView`, `DarkMode_Explorer`, `DarkMode_ItemsView` - all of
them). It is the normal Win11 Explorer details-view look, alpha-blended over
whatever background is set (~+30 lighten / darken pair; proven with a red
`LVM_SETBKCOLOR`: rules read 255,38,38 / 235,9,19). Not `LVS_EX_GRIDLINES`,
not OWNERDATA, not dark-mode ordinals, not a hidden-window latch.

Why earlier probes misled:

- An EXTERNAL `SetWindowTheme` with ANY name (even `TrayNotify`) cleared the
  rules - it demoted the control off the Explorer family, it was not a
  "re-application fixes it" signal. In-process re-apply of the same Explorer
  theme re-applies the rules, so `WM_APP_RETHEME` can never work.
- The raw-Win32 arm (`mempress_gui.cb`) and the C++ original DO get the rules;
  they hide them via custom draw: return `CDRF_NOTIFYPOSTPAINT` and overpaint
  the rule strips with the background color (`_paintOverColumnSeparators`).
- Standalone theming probes need a comctl32 v6 manifest, or `SetWindowTheme`
  is a silent no-op and every variant vacuously looks clean.

No theme name keeps the dark scrollbar but drops the rules; `pszSubIdList`
tricks (e.g. `L"SCROLLBAR"`) leave artifacts. Overpaint is the only clean fix.

## Fix direction (verified by probe, not yet applied)

In `cflat/core/ui_native/win32.cb`:

1. `_themeListView` (~line 2539): revert body theme `DarkMode_ItemsView` ->
   `DarkMode_Explorer` (the ItemsView swap fixed nothing and lost the dark
   scrollbar 23,23,23 and dark-blue selection 42,74,105). Fix the comment,
   which currently claims ItemsView avoids the rules - it does not.
2. `tryCustomDrawList` (~line 5400): when `wnd.ctlDark`, do not bail early on
   `box == 0` at `CDDS_PREPAINT` (keep that short-circuit only for
   item/subitem stages); return `CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT`
   at `CDDS_PREPAINT`; add a `CDDS_POSTPAINT` branch that fills
   `{colRight-2, headerHeight, colRight+2, client.bottom}` for EVERY column
   (including the stretched last one - live capture shows a rule at its edge
   too) with an `LVM_GETBKCOLOR` brush. `hdc` is at `p + 32` in the existing
   raw-offset NMCUSTOMDRAW style. Runs inside the control's paint cycle, so
   no flash and no show-order dependency.
3. Delete the `WM_SHOWWINDOW` -> `WM_APP_RETHEME` branches in `NativeWndProc`
   (~line 5955) and the `WM_APP_RETHEME` const - they exist only for this
   symptom and cannot work.

Full evidence dossier: `scratch/divider-investigation.md` (plus probes
`scratch/lvrule.cb`, `scratch/lvsample.ps1`, `scratch/lvdump.ps1`) - scratch
is gitignored, so this issue file is the durable record of the findings above.

## Verify

Pixel-sample the mempress body at column edges (rule pair was
`318:82,87,96 / 319:48,62,82` over bg 52,58,68): expect pure background.
Confirm scrollbar stays dark and selection stays dark blue. Gates: test.bat,
example.bat, mempress --selftest 5/5.
