# Cocoa/WinUI hosts still leave the sub-cell edge remainder unfilled

## Summary
Cell layout quantizes the window client size, leaving up to one cell of dead band
at the right/bottom edges (and above the docked status strip, whose px height is
not a cell multiple). Fixed on the Win32 host 2026-08-24: fill-style controls
(`_fillKind`) whose frame ends on the layout's last column/row absorb the
remainder when cells map to px (`_edgeWpx`/`_edgeHpx` in win32.cb), backdrops
always absorb it, and the content pane covers the full client area. The Cocoa and
WinUI hosts still map exact cell frames, so the same jumping gap (user-visible on
resize, one cell per increment) exists there.

## Repro
Run example/ui/08-fedit on macOS or the WinUI host; resize the window slowly.
The band between the editor bottom and the status bar cycles 0..cellH-1 px.

## Fix direction
Port the win32.cb pattern: a per-applyTree `fillBottomCells` (last content row =
layout height minus the status row when the tree docks a status bar), and stretch
fill-kind controls + backdrops whose cell frame ends on the last column/row to
the true client edge. Keep fixed-height chrome (buttons, fields, combos) exact.
