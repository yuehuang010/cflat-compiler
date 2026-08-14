# Win32 dark theme: trackbar channel + tick marks stay light

## Summary

Under the v6 manifest + Phase 5 theming (2026-08-14), the trackbar THUMB correctly renders
accent-colored in dark mode, but the CHANNEL (the groove) and the tick marks still render
light gray on the dark panel. Visible in scratch/theme_deep/gallery_dark_d4.bmp (Slider card).
Cosmetic only - input and thumb behave correctly.

## Repro

out\gallery.exe --shots <dir>, look at gallery_dark_d4.bmp (Slider card).

## Root cause

msctls_trackbar32 has no dark theme class (like SysTabControl32); the Explorer/CFD themes
only affect the thumb. The channel and tics come from the visual style's light parts.

## Fix direction

Trackbar supports NM_CUSTOMDRAW with per-part draw stages: handle TBCD_CHANNEL (fill the
groove with the dark track color, CDRF_SKIPDEFAULT) and TBCD_TICS in the parent's WM_NOTIFY
path in core/ui_native/win32.cb, dark mode only - same shape as the existing button
custom-draw (tryCustomDrawButton). Keep the themed thumb (CDRF_DODEFAULT for TBCD_THUMB).
