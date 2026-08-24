# WinUI ListView has no realized per-cell color rendering path

## Status

The tagged `ListCellStyle` contract, theme-role resolution, canvas fallback, and
real Win32/Cocoa cell styling are implemented. WinUI remains active because its
current mapping uses the default XAML item template for one joined row string;
`styleBox` is retained and `nativeListCellColor` reads the resolved callback, but
no realized item/cell foreground is changed.

## Evidence

The WinUI source creates a `Microsoft.UI.Xaml.Controls.ListView` and binds a
hand-written item source. It does not provide an item template, `TextBlock`
foreground binding, or realized-container styling callback. The readback driver
therefore proves callback resolution only, not visible pixels.

## Follow-up

Provide a host-owned item template/container styling path that applies the
resolved semantic color to each realized cell, while preserving virtualization
and opaque native resource ownership behind `INativeHost`.
