# Win32 menu BAR ignores the dark theme (popups fixed 2026-08-24)

## Summary
Toggling the app theme (fedit View > Theme, or system dark mode) re-themes every
control (`DarkMode_Explorer` / `DarkMode_CFD` theme classes in win32.cb
`_syncProps` paths) but the window MENU BAR and its dropdown popups stay light.
Reported by the maintainer while reviewing fedit 2026-08-24. Pre-existing - the
menu path (win32.cb `CreateMenu`/`AppendMenuA`/`SetMenu`, ~line 4624) has never
had a dark branch; unrelated to the Grid migration.

## Repro
Run example/ui/08-fedit/fedit.cb, switch the theme to dark: controls and
backdrop darken, the File/Edit/View menu bar and its popups remain light.

## Root cause
Win32 has no documented dark-menu API. The host themes controls per-HWND via
SetWindowTheme("DarkMode_*"), but menus are not HWNDs:
- Popup menus follow the process/window "preferred app mode", settable only via
  undocumented uxtheme ordinals 133/135 (AllowDarkModeForWindow /
  SetPreferredAppMode) plus 136 (FlushMenuThemes).
- The menu BAR itself does not honor even that; apps dark-theme it by handling
  the undocumented UAHDRAWMENU/UAHDRAWMENUITEM (WM_UAHDRAWMENU 0x0091/0x0092)
  owner-draw messages, or by overpainting the bar strip in WM_NCPAINT.

## Status
Maintainer ruled 2026-08-24: use the undocumented uxtheme API ("used enough
that it is effectively documented"). Step 1 LANDED same day in win32.cb
_applyDarkNonClient: SetPreferredAppMode(ForceDark/ForceLight) (ordinal 135)
+ FlushMenuThemes (ordinal 136) + DrawMenuBar on every theme change, so
DROPDOWN POPUPS now follow the app theme regardless of the system setting.

## Remaining
The menu BAR strip still paints light in dark mode - the bar renderer ignores
the app mode entirely. Fix: handle the undocumented UAHDRAWMENU /
UAHDRAWMENUITEM messages (WM_UAH* 0x0091/0x0092) in the frame WndProc and
paint bar items with theme role colors, plus overpaint the 1px bar underline
in WM_NCPAINT. Larger, self-contained; same undocumented-API ruling covers it.
