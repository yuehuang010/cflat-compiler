# Native ListView has no column-header click event

Imported issue #16 from winmempress's `COMPILER-ISSUES.md`. Triage: p3.

## Repro / use case

Create a `ListView` with two columns and a user-facing sort order. A click on a column
header should let the app select the sort key and re-render the virtual rows. Today the
app can handle row selection and activation, but cannot distinguish a header click from
any other native control activity.

## Verified current-code evidence and root cause

- `cflat/core/ui_native.cb` defines `IListView` with columns, row text, selection, and
  `fireSelect`/`fireActivate`, but has no column-click field or fire method. The
  `ListView` class has the same gap. The canvas `ListView.dispatch` handles a mouse
  event only as a row coordinate, so the header is consumed without an app callback.
- `cflat/core/ui_native/win32.cb` declares and routes `LVN_GETDISPINFOA`,
  `LVN_ITEMCHANGED`, `NM_DBLCLK`, and `LVN_KEYDOWN`. There is no column-click notify
  constant or route. `ListCtlProc` subclasses the control only to forward header
  `NM_CUSTOMDRAW` for `_drawListHeader`.
- `cflat/core/ui_native/cocoa.cb` installs row click and double-click IMPs
  (`_onListClickImp` and `_onListDoubleClickImp`) but no `NSTableHeaderView` action.
  `cflat/core/ui_native/winui.cb` exposes row drivers and the item-source adapter but
  no header event. The host-neutral driver list likewise has no column operation.

The existing `setListOp` protocol carries columns and rows, not user input from a header,
so the missing interface callback, host routing, and driver are one coherent gap.

## Scope

This is a three-native-host seam change: Win32 `WC_LISTVIEW`, Cocoa `NSTableView`, and
WinUI `ItemsView`/ListView. The `ICanvas` fallback should give header clicks a defined
model-level behavior, and canvas/headless tests need a deterministic driver rather than
requiring a real header window. Do not assume the Win32 `NM_CUSTOMDRAW` hook is portable
to the other hosts.

## Bounded fix direction / acceptance criteria

- Add a deliberately named column callback/event carrying the zero-based column index,
  with a fire wrapper and interface exposure consistent with the eventual `IListView`
  property audit. The exact callback spelling can be settled with the API design.
- Route real header clicks in all three native hosts and provide a host-neutral,
  headlessly callable driver that exercises the same model callback. A row click must
  continue to route only `onSelect`, and a double-click/Enter only `onActivate`.
- Extend an existing UI self-test to prove the callback receives the clicked column on
  Win32, Cocoa, and WinUI/headless paths where those host gates exist; no sort policy
  belongs in the framework.
