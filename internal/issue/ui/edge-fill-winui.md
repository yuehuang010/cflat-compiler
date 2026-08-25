# WinUI host still leaves the sub-cell edge remainder unfilled

## Summary
Cell layout quantizes the window client size, leaving up to one cell of dead band
at the right/bottom edges (and above the docked status strip, whose px height is
not a cell multiple). Fixed on the Win32 host 2026-08-24 (`_fillKind` +
`_edgeWpx`/`_edgeHpx` in win32.cb) and ported to the Cocoa host 2026-08-25
(points-based: per-apply remainder from the contentView frame, fill-kind controls
and backdrops ending on the last column/row stretch, StatusBar docks flush at the
true bottom via a sentinel frame in `setFrame`; verified by a scratch probe at a
non-cell-multiple window size - list right edge == contentView width, status flush
at bottom, no band between them). Only the **WinUI** host still maps exact cell
frames, so the jumping gap (user-visible on resize, one cell per increment)
remains there.

## Repro
Run a fill-style app (e.g. the fedit tree) on the WinUI host; resize the window
slowly. The band between the editor bottom and the status bar cycles 0..cellH-1 px.

## Fix direction
Port the win32.cb pattern (or mirror the cocoa.cb port, which is the smaller
diff): a per-applyTree remainder + `fillBottomCells` (last content row = layout
height minus the status row when the tree docks a status bar), stretch fill-kind
controls + backdrops whose cell frame ends on the last column/row to the true
client edge, dock the status strip flush. Keep fixed-height chrome exact.
Requires a Windows host with the Windows App SDK to build and verify winui.cb.
