# WinUI ListView has no realized column-header click path

## Status

The host-neutral `IListView.onHeaderClick(column)` contract and real Win32/Cocoa
routes are implemented. WinUI remains active because its current mapping uses one
XAML `ListView` item string per row and does not create a header control. The
`nativeListHeaderClick` helper is a deterministic driver only; it is not evidence
of a user click.

## Evidence

The WinUI source binds `WItemSource` to `ListView.ItemsSource` and joins report
columns into one string. There is no `GridViewHeaderRowPresenter`, header button,
or pointer event route in the host. A live WinUI click therefore cannot invoke the
callback.

## Follow-up

Add a host-owned header element and route its zero-based button/pointer event to
the existing boxed callback without exposing XAML types through `ui_native.cb`.
