# fireClick/fireToggle/fireSet call an unset handler -> segfault on click

Created: 2026-07-13 (found while building the doc/UI.md samples during Phase 7)

## Summary

`Button.fireClick()` calls `this.onPress()` with no null guard. A Button built
via the JSX/tag sugar WITHOUT an `onPress` therefore SEGFAULTS the first time it
is clicked. The same shape applies to `Checkbox.fireToggle()` / `Slider.fireSet()`
and the other fire* wrappers.

Pre-existing (the fire* wrappers predate the interface refactor); NOT caused by
it. Nothing in the tree hits it today because every FACTORY forces a handler
argument - but the tag sugar lets a user construct a handler-less control, and
that is a perfectly reasonable thing to write.

## Repro

```cflat
// A button with no onPress - compiles fine, crashes when clicked.
IButton b = <Button key="b" title="Click me"/>;
root.add(b);
```

Click it -> access violation inside `fireClick`.

## Root cause

`cflat/core/ui_native.cb`, the fire* wrapper methods. They exist so the hosts can
INVOKE a handler without reading the closure field (Phase 3/4 design), e.g.:

```cflat
void fireClick() { this.onPress(); }        // <-- no guard
```

An unset `Lambda` field is a null closure; calling it jumps through a null
function pointer.

## Fix direction

Guard each fire* wrapper before invoking, e.g.

```cflat
void fireClick() { if (this.onPress != nullptr) this.onPress(); }
```

Confirm first how a `Lambda<...>` field is null-tested in cflat (check
`core/function.cb` and how an unset closure is represented - the env pointer and
the tag bit). If there is no idiomatic null test for a closure, that gap is the
real issue and should be fixed first.

Touches `core/`, so it needs its own rebuild + full regate
(`test.bat` / `test_lsp.bat` / `example.bat`). Add a regression case to an
existing UI example's self-test (a handler-less Button that gets clicked), not a
new test file.
