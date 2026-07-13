# Container backgrounds + Text font variants are Cocoa-only (Win32/WinUI: no-op)

Created: 2026-07-12 (ui-visual-polish plan, Phases A + B)

## Summary

Two visual-polish features landed on the Cocoa host only; the Win32
(`core/ui_native/win32.cb`) and WinUI 3 (`core/ui_native/winui.cb`) hosts were
intentionally NOT changed (parity gap, non-silent, tracked here):

1. Container background painting (Phase A). A plain container (an `ELEM_VIEW`
   from `view`/`row`/`column`) with a non-zero `style.backgroundColor` gets a
   layer-backed `NSView` backdrop (rounded 8pt corners, optional 1px
   `theme.panelBorder`) painted behind its children, keyed by the container's
   path. See `_containerBg` / `_createBackdrop` / `_syncBackdrop` and the
   backdrop branch in `_applyNode` in `core/ui_native/cocoa.cb`.

2. Text font variants (Phase B). `Text.font` (`FONT_UI` default, plus
   `FONT_TITLE` bold ~1.3x and `FONT_CAPTION` smaller + secondary label color)
   is applied to the `NSTextField` on create and re-sync (`_applyTextFont` /
   `_fontFor`), and honored in `measureText`.

## Repro

Build an app on the Win32 or WinUI host with a `column` card whose
`style.backgroundColor` is set, plus a `Text` with `font = FONT_TITLE`:

- On Cocoa: the card paints a rounded filled panel behind its controls and the
  title renders in a bold, larger font.
- On Win32/WinUI: the card background is not painted (the `ELEM_VIEW` container
  stays geometry-only, as before), and every `Text` renders in the default
  control font regardless of `Text.font`.

## Root cause

Both features are host-specific rendering. The container backdrop needs a real
layer-backed view + document-relative framing inside a `ScrollView`; the font
variants need the toolkit's font objects. Only the Cocoa `_applyNode` /
`_syncProps` path implements them. The model (`ui_native.cb`) carries the new
`Text.font` field, `FONT_TITLE`/`FONT_CAPTION` consts, and the existing
`theme.panelBorder` slot host-neutrally, so the Win32/WinUI hosts compile and
run unchanged - they just ignore these hints.

## Fix direction

Win32: paint the container fill in the parent window's `WM_PAINT` (or a static
child with a themed brush) keyed by the container path; map `Text.font` to
`CreateFont` variants (bold/1.3x, secondary gray) and `SendMessage WM_SETFONT`.
WinUI: set a `Border`/`Grid` `Background` for the container and a `TextBlock`
`FontWeight`/`FontSize`/`Foreground` for the font variants. Neither blocks the
Cocoa work; add when Windows visual parity is prioritized.
