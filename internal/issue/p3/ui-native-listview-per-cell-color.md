# Native ListView has no per-cell or per-row color data

Imported issue #17 (color half) from winmempress's `COMPILER-ISSUES.md`.
Triage: p3. The icon half is tracked separately in
`internal/issue/p4/ui-native-listview-row-icons.md`.

## Repro / use case

Build a report-style `ListView` whose cells communicate status (for example, red
errors, amber warnings, and normal rows). `rowText(row, col)` can supply text only; there
is no way to supply a row or cell color while retaining virtualization.

## Verified current-code evidence and root cause

`cflat/core/ui_native.cb` gives `ListView` only `columns`, `colWidths`, `rowCount`,
`rowText`, selection, and activation. `LISTOP_*` has no color operation. The canvas
fallback joins each row into one string and paints it with one `ink` chosen from the
theme. `ListView` therefore has no per-row or per-cell style state at the model seam.

The Win32 backend already has `NM_CUSTOMDRAW` machinery, but it is currently used for
the ListView header (`ListCtlProc` and `_drawListHeader`) and selected control accents,
not virtual row/cell data. Cocoa's `NSTableView` data source currently returns strings
through `_tableObjectValueImp`, with no cell view/delegate color path. WinUI's item
source joins the columns into one display string and has no row/cell brush payload.

## Scope

The change spans Win32, Cocoa, and WinUI; WinUI cannot reuse Win32 `NM_CUSTOMDRAW` and
needs a XAML item/cell styling mechanism. The `ICanvas` fallback needs a defined color
callback/rendering rule as well, while canvas/headless readback should remain
deterministic and must not depend on a screenshot-only assertion.

## Bounded fix direction / acceptance criteria

- Design one virtualization-safe color contract, explicitly deciding whether the first
  version is per-row, per-cell, or supports both. Keep the callback/data shape bounded;
  do not copy 100k rows into host-owned style lists.
- Carry the chosen color data through the existing list update seam or a narrowly scoped
  companion operation, and implement equivalent visible-cell behavior in all three
  native hosts plus the canvas fallback. Preserve the default theme when no color is
  supplied.
- Add an existing related UI self-test that checks representative default, colored, and
  changed rows/cells through a host-neutral readback or model assertion. Verify that
  invalidation updates colors without rebuilding the row source and that teardown is
  leak-clean.
