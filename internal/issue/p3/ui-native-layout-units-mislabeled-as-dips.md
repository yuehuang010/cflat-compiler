# Native UI layout units are cells mislabeled as DIPs

Imported issue #19 from winmempress's `COMPILER-ISSUES.md`. Triage: p3.
This is primarily a documentation and naming correction; a diagnostic warning is
optional and should not be assumed by this issue.

## Repro / use case

Read the native layout documentation and size a control with `Style.width`,
`ListView.colWidths`, or `setFrame`. The documentation and parameter names say DIPs,
but a layout integer is a framework cell that is converted using host-specific
`BASE_X`/`BASE_Y`. Treating `width = 100` as 100 physical-independent pixels therefore
produces an unexpectedly wide control and makes cross-host sizing difficult to reason
about.

## Verified current-code evidence and root cause

- `doc/UI.md` currently says native hosts read layout integers as DIPs and scale them at
  the boundary. `INativeHost.setFrame` calls its parameter `frameDip`, and
  `LISTOP_ADD_COLUMN` plus `IListView.colWidths`/`addColumn(..., widthDip)` use DIP names.
- `cflat/core/ui_native/win32.cb` defines `BASE_X = 8`, `BASE_Y = 26`; `dipX()` and
  `dipY()` scale those bases by monitor DPI, and `Window.setFrame` multiplies layout
  integers by the resulting values.
- `cflat/core/ui_native/cocoa.cb` multiplies frames and content sizes by `BASE_X` and
  `BASE_Y` with no DPI term. `cflat/core/ui_native/winui.cb` similarly multiplies
  layout dimensions by its `BASE_X`/`BASE_Y` values and scale.

Thus the current integer is a cell in the shared layout model, not a DIP. The TUI and
other `ICanvas`/headless paths already consume the same integer as a cell. The imported
report was right about the mismatch, but the native seam covers three hosts, not two.

## Scope

Correct the user-facing terminology across `doc/UI.md`, interface parameter comments,
and the list-column names while preserving the existing numerical behavior on Win32,
Cocoa, WinUI, canvas, and headless tests. Do not silently change layout scale as part
of this documentation issue.

## Bounded fix direction / acceptance criteria

- Document the shared unit as cell/layout units, state each host's cell-to-DIP/point/
  pixel conversion, and distinguish the conversion from physical DPI scaling.
- Rename misleading public parameter/comments where compatibility permits (including
  `frameDip`, `widthDip`, and `colWidths` terminology), or explicitly document any
  retained source spelling. Update the native resize/test-driver wording consistently.
- Add a small documentation or existing UI assertion that demonstrates one known cell
  size on Win32, Cocoa, WinUI, and canvas/headless paths. An order-of-magnitude warning
  may be considered separately, but is not required for this issue to be complete.
