# Canvas input handlers + canvas image handles: DONE on Win32 + WinUI 3; still open on the GDI canvas host

Created: 2026-07-12 (ui-map-canvas plan, M1 + M2)
Updated: 2026-07-13 - canvas PAINT, IMAGES and INPUT all landed on the WinUI 3 host
(`core/ui_native/winui.cb`), after the same three landed on the Win32 native host earlier the
same day. Remaining gap: the GDI canvas host (`core/ui_canvas/win32.cb`).

## Summary

Two CanvasView features originally landed on the Cocoa host only. Both are now wired on the
Win32 NATIVE host and on the WinUI 3 host as well.

1. Canvas image handles (M2) - DONE on Cocoa + Win32 native + WinUI. `canvasCreateImage` copies
   the caller's top-down BGRA32 buffer into a 32bpp DIB section and returns the HBITMAP (so the
   caller may free its pixels immediately, mirroring the Cocoa NSBitmapImageRep copy);
   `drawImage` recovers the source extent with `GetObjectA` and `StretchBlt`s into the paint DC;
   `canvasReleaseImage` deletes the bitmap. No side registry is kept, so nothing is tracked on
   the cflat heap (a `list<>` registry would leak its backing buffer at exit and trip
   `--heap-audit`).

2. Interactive CanvasView input (M1) - DONE on Cocoa + Win32 native + WinUI. Pointer down/move/up,
   wheel, and (ctrl-wheel) pinch reach the element's `onPointerDown/Move/Up` / `onWheel` /
   `onPinch`. The host-neutral drivers (`nativeCanvasPointer/Wheel/Pinch`) are real on all three.

### Three things AppKit gives for free that both Windows hosts had to be taught

- **Drag capture.** Cocoa keeps routing `mouseDragged:`/`mouseUp:` to the `mouseDown:` view once a
  drag starts. Win32 stops sending mouse messages the moment the cursor leaves the child
  (`SetCapture`/`WM_CAPTURECHANGED` reproduce it); XAML likewise routes to the element UNDER the
  pointer, so the WinUI host calls `CapturePointer` on `PointerPressed` and
  `ReleasePointerCaptures` on `PointerReleased`, and subscribes `PointerCaptureLost` to synthesize
  a pointer-up so an app cannot be stranded mid-drag.
- **Wheel delivery.** Cocoa sends `scrollWheel:` to the view under the cursor. Win32 sends
  `WM_MOUSEWHEEL` to the FOCUSED window, so `NativeWndProc` hit-tests the cursor against the
  window's canvas children (`_routeCanvasWheel`). XAML needs none of this: `PointerWheelChanged`
  is a routed event delivered to the element under the pointer.
- **Pinch.** Cocoa has `magnifyWithEvent:`. On Windows a precision touchpad's pinch-zoom is
  delivered as CTRL + wheel by platform convention, so a ctrl-held vertical wheel dispatches to
  `onPinch` (scale = 1.0 + lines*0.25) on BOTH Windows hosts rather than to `onWheel`.

Wheel deltas are reported in LINES (WHEEL_DELTA = 120) on both Windows hosts, and `precise` is
always false there (neither Win32 nor the XAML `PointerPointProperties` exposes a
trackpad/precise-delta distinction), which is what makes the map ZOOM on wheel.

## How the WinUI CanvasView is drawn (no Win2D)

WinUI 3 is retained-mode XAML with no immediate-mode drawing surface, and taking a Win2D
dependency was explicitly rejected. ELEM_CANVAS is therefore a
`Microsoft.UI.Xaml.Controls.Image` backed by a `WriteableBitmap`: the host rasterizes the app's
boxed `onPaint` closure into an offscreen top-down 32bpp GDI DIB section (`WinuiGdiCanvas`, a
verbatim port of the Win32 host's `NativeGdiCanvas`), copies the DIB bits into the bitmap's
`PixelBuffer` (through `IBufferByteAccess`, the plumbing `setImageData` already had) and calls
`Invalidate()`. `_syncProps` re-runs this on every pump - the WinUI equivalent of the Win32
host's `InvalidateRect`.

Three things worth knowing before touching this code:

- **GDI never writes the alpha channel.** The DIB comes back with A = 0, and a WriteableBitmap is
  premultiplied BGRA, so an unmodified copy renders FULLY TRANSPARENT (you see nothing and
  conclude the paint failed). The upload forces every pixel's alpha byte to 0xFF.
- **GDI is bound at runtime, not linked.** winui.cb must not import windows.h (its types collide
  with the WinUI winmd projection), and a `lib` clause has no home in this file - a
  `package-nuget` import takes none, and a `lib` clause on a `.cb` import is silently ignored by
  `CompileImportedFile`. gdi32/user32 therefore cannot be put on the link line, so the ~19 entry
  points are resolved with `LoadLibraryA`/`GetProcAddress` (kernel32 is linked by default). This
  keeps the hand-declared extern surface at two functions.
- **Coordinates.** The WinUI host uses the same CELL model as Win32/Cocoa (`BASE_X = 8`,
  `BASE_Y = 34`): `setFrame` scales cells to DIPs, and there is no dpi factor in the LAYOUT (DIPs
  are already dpi-independent). Before this, `measureText` returned DIPs while `setFrame` passed
  layout units through 1:1 - two unit systems in one layout pass, which crammed every control
  into a tiny top-left cluster and made the canvas a 24x6px smudge. `BASE_Y` is 34, not the Win32
  host's 26: a XAML control is chunkier than its GDI equivalent (a default Button is 32 DIPs), so
  a 1-cell control row clipped its own text at 26.
  (`measureText` is dead code on every host - the layout engine never calls it.)
- **The canvas bitmap is DEVICE resolution, not DIPs.** The WriteableBitmap behind a canvas is
  sized `cells * BASE * rasterizationScale` px and `WinuiGdiCanvas.scale`/`cellW`/`cellH` are
  scaled to match, so the canvas rasterizes at native pixels and XAML downscales it into the same
  DIP box. Sizing it 1px-per-DIP renders a blurry, half-resolution canvas on a >100% monitor.
  Pointer coordinates stay in DIPs (that IS the canvas "points" unit) - do not scale them.

## Process DPI awareness (the bug that made everything look wrong)

WinUI 3 normally gets PerMonitorV2 DPI awareness from an **app manifest**, which a cflat-generated
exe does not have. The host therefore came up DPI-UNAWARE: Windows lied to it (`GetDpiForWindow`
returned 96 on a 192-dpi monitor) and bitmap-stretched the whole window 2x, so text was blurry,
every control was double-size, and content overflowed the window. `runAppWinui`/`runWinuiHeadless`
now call `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` before any window or XAML init -
the same thing the Win32 host has always done (`win32.cb`). It is bound at runtime via
`LoadLibraryA`/`GetProcAddress`, since winui.cb cannot import windows.h or link user32 (below).

Verified on a 200%-scale monitor: awareness goes 0 (UNAWARE) -> 2 (PER_MONITOR), `GetDpiForWindow`
96 -> 192, and the window reports its true 2160x1241 physical extent instead of a virtualized
1080x602. Note a capture/probe process must ITSELF be per-monitor-aware or Windows hands it back
virtualized coordinates for a DPI-aware target window.

## The live WinUI window's size (fixed alongside the cell model)

`runAppWinui` never learned the real client size - `_wW/_wH` stayed at their 80x40 default - so a
live app laid out into an 80x40-cell box no matter how big the window was. The window content is
now a **Grid** wrapping the control Canvas, and the host subscribes the Grid's `SizeChanged`:

- A Canvas cannot report the client size. As `Window.Content` it has no arranged size - its
  `ActualWidth` reads 0 and its `SizeChanged` never fires (measured, not assumed). A Grid
  stretches to the client area, so both work.
- `Window.SizeChanged` itself is NOT usable from cflat: it is a generic
  `TypedEventHandler<object, WindowSizeChangedEventArgs>`, and `winrtDelegate` only projects
  non-generic delegates ("is not an imported WinRT delegate type").
- On a size change the host **rebuilds** the tree (`buildWinuiTree`), it does not merely
  relayout: an app's `render()` reads `hostWidth()/hostHeight()`, and a reconcile keeps the
  mounted node's props, so a fresh render is the only way the new extent reaches the tree. The
  Win32 host rebuilds on WM_SIZE for the same reason.
- `startHeadlessWindow(app, title, w, h)` keeps its contract: w/h are CELLS (matching Win32 and
  Cocoa), and the headless path deliberately does not subscribe SizeChanged - a self-test's
  extent is the one it asked for.

## Verification

- `example/ui/09-map/map.cb --probe` is the live run-loop oracle on Win32 (exit 0 only if every
  visible tile resolved on screen); it passes.
- Win32 WndProc routing (capture, wheel hit-test, ctrl-wheel pinch) was verified by sending real
  `WM_LBUTTONDOWN`/`WM_MOUSEMOVE`/`WM_LBUTTONUP` and (ctrl-)`WM_MOUSEWHEEL` into the live window
  and reading the map's HUD back.
- WinUI: a scratch headless probe under the real WindowsAppSDK runtime confirmed the paint
  closure IS invoked (and re-invoked after `ctx.invalidate()`), that `canvasCreateImage` +
  `drawImage` return/blit without fault, and that `nativeCanvasPointer/Wheel/Pinch` drive the
  element handlers with the right coordinates. `winui_gallery.cb`'s gate (which mounts a
  CanvasView card) stays at its pre-existing 38/39.

## ScrollView + container backdrops (closed 2026-07-13)

Two more Cocoa-only features are now on both Windows hosts, which is what took `example.bat` from
88/2 to 90/0:

- **Container backdrop.** A plain `ELEM_VIEW` with a non-zero `style.backgroundColor` gets a native
  control keyed by the CONTAINER's own path (this is what the gallery's `card backdrop handle
  exists` assertion checks). Win32 uses a STATIC painted via `WM_CTLCOLORSTATIC` from a color-keyed
  brush cache; WinUI uses a `Shapes.Rectangle` at `Canvas.ZIndex = -1`. Two traps: on Win32,
  `HWND_BOTTOM` alone is NOT enough - z-order sets paint ORDER, but an unclipped bottom sibling
  still repaints its whole rect over the controls, so the backdrop needs `WS_CLIPSIBLINGS`. On
  WinUI, `Microsoft.UI.Xaml.CornerRadius` is a 32-byte value struct that the thin-vtable path does
  not coerce, so a `Border` is unusable here - hence the Rectangle with scalar `RadiusX/Y`.
- **ScrollView (`ELEM_SCROLL`).** Previously mapped on Cocoa only, so a Windows app clipped
  everything below the window bottom with no scrollbar. WinUI wraps the control Canvas in a
  `ScrollViewer` (Grid > ScrollViewer > Canvas) and sizes the Canvas from the root's `contentH`;
  XAML then supplies wheel/scrollbar/trackpad for free. Win32 owns the offset itself: `WS_VSCROLL`
  + `SetScrollInfo` + `WM_VSCROLL`, subtracting the offset at the single `DeferWindowPos`
  positioning seam. The wheel is shared - `_routeCanvasWheel` still wins, and the page only scrolls
  when no CanvasView consumed the notch (the map's `--probe` still passes 8/8).
- **A Win32 re-entrancy leak found on the way:** `ShowScrollBar` sends a SYNCHRONOUS `WM_SIZE`,
  which re-entered `buildCurrentTree` before `wnd.ready` was set and orphaned the outer tree
  (1484 leaked allocations under `--heap-audit`). Guarded with a `_building`/`_rebuildPending` pair.
- **The backdrops exposed a latent WinUI theme bug.** The host computed the cflat theme's darkness
  but never pushed it into XAML, so controls inherited the SYSTEM theme (white ink under a dark
  system theme) while the backdrops filled from the app's LIGHT theme - white on white. The root
  Grid's `RequestedTheme` + page Background are now synced from the theme on every reconcile. Note
  a code-side `ResourceDictionary` lookup of `ApplicationPageBackgroundThemeBrush` resolves against
  the APP theme at lookup time (it is not a `{ThemeResource}` binding), so it does NOT follow
  `RequestedTheme` - the page fill is derived from `theme.panelBg` and re-synced instead.

## GDI canvas host images (closed 2026-07-13)

`core/ui_canvas/win32.cb` was the last host with stub image handles (`drawImage` a no-op,
`canvasCreateImage` returning a DUMMY 1u). It now uses the same DIB-section + `GetObjectA` +
`StretchBlt` approach as the native host, with no side registry (a `list<>` handle registry leaks
its backing buffer and trips `--heap-audit`). NOTE the unit difference: this host has no dpi/points
scale factor at all - `drawText`/`drawRect` multiply cells by the raw `CELL_W`/`CELL_H` pixel
constants - so `drawImage`'s point coordinates are device pixels 1:1 here, unlike the native host's
`NativeGdiCanvas.scale`. Nothing in `example/ui/03-canvas-win32/` draws an image, so this was
verified with a throwaway probe that blits a procedural image and pixel-samples the result.

## Remaining

1. No automated gate exercises the OS input-routing layer on either Windows host: the Win32
   canvas WndProc (capture, wheel hit-test, ctrl-wheel pinch) and the WinUI
   `PointerPressed/Moved/Released/CaptureLost/WheelChanged` subscriptions are only reachable from
   real OS input. The headless drivers call the element handlers directly, so a regression in the
   routing layer would be silent. DECIDED (user, 2026-07-13): leave this manually verified rather
   than add a test file. To re-verify by hand, inject REAL input - a synthetic
   `SendMessage(WM_MOUSEWHEEL)` does NOT reach a XAML island (it takes pointer input off the real
   input stack), so use `mouse_event`/`SendInput` with the window foregrounded, or the Win32 HUD
   readback described above.
2. Pinch is ctrl-wheel only on BOTH Windows hosts - there is no `PinchGestureRecognizer` /
   touch-manipulation path, so a two-finger pinch on a TOUCH SCREEN does not reach `onPinch` (a
   precision-touchpad pinch does, since Windows delivers it as ctrl-wheel). Deferred: no touch
   hardware on the dev box, so it could be written but not verified.
3. A WinUI canvas is re-rasterized on EVERY reconcile - any `ctx.invalidate()` anywhere in the app
   repaints every canvas. It cannot be skipped: `CanvasView` carries no dirty/revision flag and its
   `onPaint` is a fresh `Lambda` each render, so a host cannot tell whether the drawing changed.
   Skipping would need a revision counter on the element (a public API change across all hosts) -
   deliberately not done. The per-paint cost was instead reduced ~10% (measured, 200 invalidate+
   pump cycles over an 800x680 canvas) by caching the memory DC and the background brush across
   paints and folding the alpha-forcing pass into a single 32-bit word copy. The remaining cost is
   the app's own draw calls plus the WriteableBitmap upload, not GDI object churn. The
   WriteableBitmap is still recreated when the canvas's DIP box changes size, and a window resize
   still rebuilds the whole tree (which is the Win32 behaviour, and is required because `render()`
   reads `hostWidth()`).
4. No automated gate covers WinUI DPI or the rendered pixels. The headless self-test has no live
   `XamlRoot`, so the rasterization scale is always 1.0 there and the device-resolution canvas
   path is never exercised; process DPI awareness is likewise only observable on a live window.
   Both were verified by hand on a 200%-scale monitor (foreground the window, capture it from a
   per-monitor-aware process, and read the awareness/DPI back). `PrintWindow` cannot capture
   WinUI's DirectComposition content (it returns chrome + a black client area), so the capture
   must come off the screen with the window forced topmost. A real in-process pixel oracle would
   need `RenderTargetBitmap`, which is async and blocked on WinRT delegate projection for the
   completion handler.
