# Native ListView has no row-icon support

Imported issue #17 (icon half) from winmempress's `COMPILER-ISSUES.md`.
Triage: p4. Per-row/cell colors from that report are tracked separately in
`internal/issue/p3/ui-native-listview-per-cell-color.md`.

## Repro / use case

Create a file or process list where each row needs a status/type icon alongside its
virtualized text. `ListView` has no icon field or row callback, so the only workaround is
to encode an icon as text or abandon the native list.

## Verified current-code evidence and root cause

`cflat/core/ui_native.cb` exposes `rowText(row, col)` and the `LISTOP_*` row/column
operations, but no image or icon payload. The existing `Image` element is a standalone
control uploaded by `INativeHost.setImageData`; it is not an image-list contract for
virtual rows. `ListView.paint` on the canvas also renders only joined text.

Win32 `WC_LISTVIEW` would need an image list or owner-data item image path with explicit
ownership and destruction. Cocoa's `NSTableView` currently returns strings from its data
source and has no image-bearing cell view/delegate path. WinUI's item source is a joined
row string, so an icon needs a XAML data template or equivalent. These are resource
lifetime and virtualization concerns, not just another integer `LISTOP`.

## Scope

The seam must cover Win32, Cocoa, and WinUI. The `ICanvas` fallback should have a
documented icon representation or deterministic omission rule, and canvas/headless
tests must not require a native image handle. An image-list/cache lifetime must be
explicit for controls that outlive a render pass.

## Bounded fix direction / acceptance criteria

- First design the resource model: stable icon identity/bitmap ownership, cache lifetime,
  replacement, and destruction when a ListView or row source is torn down. Avoid passing
  toolkit-specific handles or per-row owning images through `rowText`.
- Implement visible-row icon resolution in all three native hosts and a bounded canvas
  fallback, with a clear behavior for missing icons and for virtualization/reuse.
- Add a host-neutral/headless exercise for icon identity and teardown, plus native host
  checks where available. Confirm that scrolling/reloading does not leak image resources
  or retain obsolete row callbacks.
