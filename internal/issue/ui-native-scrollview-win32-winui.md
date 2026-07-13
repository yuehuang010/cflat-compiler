# ScrollView is native-mapped on Cocoa only (Win32/WinUI: plain container)

Created: 2026-07-12 (during the ui_native scroll milestone)

## Summary

`ScrollView` (`ELEM_SCROLL`) now maps to a real `NSScrollView` on the Cocoa
host (wheel/trackpad/scrollbars, native document view), added in
`core/ui_native/cocoa.cb`. The Win32 (`core/ui_native/win32.cb`) and WinUI 3
(`core/ui_native/winui.cb`) hosts were intentionally NOT changed: on those
hosts `ELEM_SCROLL` is not in `nativeMappable()`, so it behaves as a plain
layout container - its children are positioned at their absolute frames, but
there is no viewport clipping and no native scrolling. Content taller than the
window simply overflows / is clipped by the window with no scrollbar.

The framework-level (TUI/canvas) ScrollView is unchanged and still scrolls via
`scrollY` + the Canvas clip seam when it holds framework focus.

## Repro

Build an app (Win32 or WinUI host) whose root is a `ScrollView` with
`style.height == 0` holding more rows than fit the window:

```cflat
ScrollView* root = scrollView(makeStyle(0, 0, 0));
// ... add 40 rows taller than the client area ...
```

On Cocoa: a scrollbar appears and the page scrolls. On Win32/WinUI: the rows
past the window bottom are just not visible; there is no scrollbar and no wheel
scrolling.

## Root cause

The native-scroll mapping is host-specific: it needs the host's real scroll
control (`NSScrollView` + flipped document view on Cocoa) plus document-relative
child framing and document-view sizing from `ScrollView.contentH`. That logic
was implemented only in the Cocoa `_applyNode` / `createControl` path. Win32 and
WinUI `nativeMappable()` still exclude `ELEM_SCROLL`, so the reconciler treats
the node as a geometry-only container.

## Fix direction

Mirror the Cocoa work in each host:

- Win32: create a child window with `WS_VSCROLL` (or a container that owns a
  scrollbar) as the scroll node's control, parent descendant controls to it,
  make their frames document-relative (subtract the scroll node's origin), size
  the scrollable extent to `ScrollView.contentH`, and handle `WM_VSCROLL` /
  `WM_MOUSEWHEEL` to move the child-window scroll position. Thread a
  parent-HWND + offset through `_applyNode` (the Cocoa host added a
  `parentView` + `offX`/`offY` parameter for exactly this).
- WinUI: wrap the children in a `ScrollViewer` and parent the descendant
  elements into its content panel; document-relative framing as above.

Both are one-level-of-nesting only, matching the Cocoa milestone (nested
ScrollViews are out of scope). See `core/ui_native/cocoa.cb`
(`createControl` `ELEM_SCROLL` branch, `_applyNode` `ELEM_SCROLL` branch) and
`layoutRootBounded` in `core/ui_native.cb` for the reference implementation.
