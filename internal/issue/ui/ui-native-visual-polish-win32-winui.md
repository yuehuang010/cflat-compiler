# Text font variants (FONT_TITLE / FONT_CAPTION) are Cocoa-only (Win32/WinUI: no-op)

Created: 2026-07-12 (ui-visual-polish plan, Phases A + B)
Updated: 2026-07-13 - narrowed. Phase A (container backgrounds) is now DONE on
Win32 and WinUI; only the Phase B font variants remain.

## Summary

`Text.font` (`FONT_UI` default, plus `FONT_TITLE` bold ~1.3x and `FONT_CAPTION`
smaller + secondary label color) is honored only on the Cocoa host, where it is
applied to the `NSTextField` on create and re-sync (`_applyTextFont` / `_fontFor`)
and accounted for in `measureText`. The Win32 (`core/ui_native/win32.cb`) and
WinUI 3 (`core/ui_native/winui.cb`) hosts ignore it: every `Text` renders in the
default control font.

The model (`core/ui_native.cb`) carries the `Text.font` field and the
`FONT_TITLE` / `FONT_CAPTION` consts host-neutrally, so both Windows hosts compile
and run unchanged - they just ignore the hint.

RESOLVED, removed from this issue: container background painting (Phase A). A
container with a non-zero `style.backgroundColor` now paints on all three hosts -
Win32 via a backdrop `STATIC` keyed by the container path (`_containerBg` /
`_createBackdrop` / `_syncBackdrop` / `backdropBrush`, with a z-order sink that
keeps the fill behind the card's controls), WinUI via a panel `Background`.

## Repro

Build an app on the Win32 or WinUI host with a `Text` whose `font = FONT_TITLE`:
on Cocoa the title renders bold and larger; on Win32/WinUI it renders in the plain
default control font.

## Root cause

Host-specific rendering: the font variants need the toolkit's font objects, and only
the Cocoa `_applyNode` / `_syncProps` path builds them.

## Fix direction

- Win32: build `CreateFont` variants off the current UI font (bold + ~1.3x for
  FONT_TITLE; smaller + a secondary gray for FONT_CAPTION), cache them per window
  next to `Window.font`, and push with `WM_SETFONT` from `_syncProps`. FONT_CAPTION's
  secondary ink rides the `WM_CTLCOLORSTATIC` path, so it needs a per-control color
  override rather than the single `themeText`.
- WinUI: set `FontWeight` / `FontSize` / `Foreground` on the `TextBlock`.

Neither blocks anything else; do it when Windows visual parity is prioritized.
