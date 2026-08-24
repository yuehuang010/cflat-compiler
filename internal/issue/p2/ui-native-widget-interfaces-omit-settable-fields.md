# Native widget interfaces omit settable class fields

Imported issue #18 from winmempress's `COMPILER-ISSUES.md`. Triage: p2.

## Repro / use case

Call the documented `listView(rowCount, rowText)` factory, retain its returned `IListView`,
and configure the widget through the interface (as JSX and component code do for other
widgets):

```cflat
IListView list = listView(10, rows);
list.width = 80;
list.height = 20;
list.onSelect = (int row) => { /* update model */ };
list.onActivate = (int row) => { /* open row */ };
```

The class has these fields, but the interface does not, so the interface-typed code
cannot wire the widget and reports a misleading missing-field diagnostic.

## Verified current-code evidence and root cause

`cflat/core/ui_native.cb` defines `IListView` with `columns`, `colWidths`, `rowCount`,
`selectedIndex`, `multiSelect`, `rowText`, `addColumn`, `fireSelect`, and `fireActivate`.
The concrete `ListView` additionally defines `width`, `height`, `onSelect`, and
`onActivate`. The factory returns `move IListView listView(...)`, making the omission
load-bearing for normal callers.

The omission is inconsistent with adjacent interfaces: `ITabControl` includes `width`
and `height`, while `ISplitView` includes the settable `onRatioChange` field and comments
that a caller holding `ISplitView` must be able to wire it. The general header comment
about fire wrappers describes host invocation, but does not justify hiding an app-settable
callback. This is an interface surface audit gap, not a native-host event-routing bug.

## Scope

The interface correction is host-neutral, but the fields feed all three native hosts
(Win32, Cocoa, and WinUI) and the `ICanvas`/headless implementation. Preserve the
existing ownership and closure-boxing rules when exposing fields; do not add a second
callback path that diverges from `fireSelect`/`fireActivate`.

## Bounded fix direction / acceptance criteria

- Audit `IListView` against `ListView` and expose the missing settable `width`, `height`,
  `onSelect`, and `onActivate` fields with the same types and ownership semantics as the
  class. Reconcile any other adjacent interface omissions discovered by that focused
  audit, without broad speculative API expansion.
- Ensure JSX/interface-typed code can set the fields and that native row selection and
  activation invoke the assigned closures on Win32, Cocoa, and WinUI; canvas/headless
  dispatch must invoke the same wrappers.
- Add or extend an existing UI test to cover factory-returned `IListView` configuration,
  re-render/reconcile retention, and callback delivery. The existing native list
  selection/activation behavior must remain unchanged when callbacks are unset.
