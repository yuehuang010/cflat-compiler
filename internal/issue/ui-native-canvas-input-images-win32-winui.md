# Canvas input handlers + canvas image handles are Cocoa-only (Win32/WinUI: no-op)

Created: 2026-07-12 (ui-map-canvas plan, M1 + M2)

## Summary

Two CanvasView features landed on the Cocoa host only; the Win32
(`core/ui_native/win32.cb`, `core/ui_canvas/win32.cb`) and WinUI 3
(`core/ui_native/winui.cb`) hosts were intentionally NOT wired (parity gap,
non-silent, tracked here):

1. Interactive CanvasView input (M1). `CanvasView` gained opt-in pointer / wheel /
   pinch handlers (`onPointerDown/Move/Up`, `onWheel`, `onPinch`, installed via
   `setOn*` + `has*` flags, coordinates in POINTS relative to the canvas origin).
   The Cocoa host routes real OS input: `CfCanvasView` overrides
   `mouseDown:/mouseDragged:/mouseUp:/scrollWheel:/magnifyWithEvent:` and invokes
   the resolved element handler (see `_canvasMouseDownImp` etc. in
   `core/ui_native/cocoa.cb`). The host-neutral test drivers
   (`nativeCanvasPointer/Wheel/Pinch`) are defined by the Cocoa host and the TUI
   host (`core/ui_canvas/term.cb`); the Win32 native host defines them as no-ops.

2. Canvas image handles (M2). `canvasCreateImage(bgraPixels, w, h)` /
   `canvasReleaseImage(img)` (host seam) + `Canvas.drawImage(img, dx,dy,dw,dh)`
   (POINTS, linear scaling). Cocoa wraps the copied BGRA bytes into an `NSImage`
   once and blits with `drawInRect:...:respectFlipped:YES` into the
   `CfCanvasView` context (`CocoaCanvas.drawImage`). The Win32 canvas classes
   (`GdiCanvas`, `NativeGdiCanvas`) implement `drawImage` as a documented no-op,
   and `canvasCreateImage` returns a nonzero DUMMY handle so library code runs.

## Repro

Build an app on the Win32 or WinUI host with a `CanvasView` that installs
`setOnPointerDown`/`setOnWheel` and paints a `canvasCreateImage` handle via
`drawImage`:

- On Cocoa: dragging pans (pointer handlers fire), the trackpad scroll/pinch
  reach `onWheel`/`onPinch`, and the image blits into the canvas.
- On Win32/WinUI: no pointer/wheel/pinch events reach the handlers (the child
  window does not route them to the element), and `drawImage` paints nothing
  (`canvasCreateImage` hands back a sentinel that is never realized to an HBITMAP).

## Root cause

Both features are host-specific: input routing needs the toolkit's event plumbing
mapped onto the CanvasView's boxed/element handlers, and image blitting needs the
toolkit to wrap pixels into a retained bitmap (HBITMAP / WriteableBitmap) and draw
it. Only the Cocoa host implements them. The model (`ui_native.cb`) carries the
`onPointer*`/`onWheel`/`onPinch` fields + `has*` flags, the `setOn*` installers,
`Canvas.drawImage`, and the `canvasCreateImage`/`canvasReleaseImage` seam
host-neutrally, so the Win32/WinUI hosts compile and run unchanged - a
CanvasView with no handlers behaves exactly as before, and image calls are inert.

## Fix direction

Win32: on the CanvasView child window's `WndProc`, translate
`WM_LBUTTONDOWN/MOUSEMOVE/LBUTTONUP` (client px -> points) into the element's
pointer handlers, `WM_MOUSEWHEEL`/`WM_MOUSEHWHEEL` into `onWheel`
(`GET_WHEEL_DELTA_WPARAM`; no precise-delta concept), and `WM_GESTURE`
(GID_ZOOM) into `onPinch`. For images, build a top-down 32bpp DIB section from
the BGRA bytes in `canvasCreateImage` (return the HBITMAP as the handle) and
`StretchBlt`/`AlphaBlend` it in `drawImage`; free the DIB in
`canvasReleaseImage`. WinUI: route `PointerPressed/Moved/Released`,
`PointerWheelChanged`, and a `PinchGestureRecognizer` to the handlers; back
images with a `WriteableBitmap` drawn via the canvas's composition/Win2D surface.

Neither blocks the map example, which is native-host-first; the example will
SKIP on Win32/WinUI until this is closed.
