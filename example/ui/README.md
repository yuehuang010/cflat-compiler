# CFlat UI examples

A tutorial for the `ui_native` framework, in chapters. Each chapter builds on the one
before it, so read them in order; within a chapter, read the files in the order listed.

The framework and every host ship in `core/` and deploy next to `cflat.exe`, so most
examples need no `-i` flag. The full reference is [`doc/UI.md`](../../doc/UI.md).

| Chapter | What it teaches |
|---------|-----------------|
| [01-elements](01-elements/) | The element model, on its own - no window, no host |
| [02-terminal](02-terminal/) | The first host: drawing the tree into a terminal |
| [03-canvas-win32](03-canvas-win32/) | The same trees on a Win32 GDI canvas |
| [04-native-controls](04-native-controls/) | The `NativeHost` seam: real OS widgets, not pixels |
| [05-gallery](05-gallery/) | The whole widget set, in one host-neutral component |
| [06-winui](06-winui/) | A third host: WinUI 3 / XAML |
| [07-testing](07-testing/) | Driving a UI app from an automated test |
| [08-fedit](08-fedit/) | The flagship: a small native text editor |

## 01-elements

Build an `Element` tree, render it, and fire a handler - all headless, so these run
anywhere with `--run` and print their results.

- `counter.cb` - a `Component` with state, rendered with `view()`/`text()`/`button()`.
- `counter_jsx.cb` - the same tree in the `<View/>` sugar, asserted identical to the above.
- `app.cb` - composing components: a parent owns children and renders them as keyed nodes,
  so child state survives re-render, insertion, removal, and reorder.

```bash
x64/Release/cflat.exe example/ui/01-elements/counter.cb --run
```

## 02-terminal

The first real host. `core/ui_canvas/term.cb` paints the tree into a double-buffered
terminal and dispatches mouse and key input back into it.

- `tui_demo.cb` - the demo plus a headless reconciler/layout/dispatch self-test suite.
- `boxes.cb` - an interactive resizable bordered-box demo; click to nest a box.

Both are interactive on a real console and fall back to a deterministic headless
self-test when I/O is redirected (which is how `example.bat` gates them).

```bash
x64/Release/cflat.exe example/ui/02-terminal/tui_demo.cb --run
```

## 03-canvas-win32

Swap the draw target, keep everything above it. `core/ui_canvas/win32.cb` is a GDI
canvas: the tree, the reconciler, and the model layer are unchanged from chapter 02.

- `win32_settings.cb` - the widget set (labels, text field, checkboxes, slider) plus
  live light/dark theming.
- `win32_boxes.cb` - `boxes.cb` from chapter 02, rendered to a real window.
- `win32_shot.cb` - paints the tree into an offscreen DC and writes a BMP, so the UI
  can be seen from a headless build.

## 04-native-controls

Above the `NativeHost` seam nothing changes; below it, every widget becomes a genuine
OS control. The OS paints them, handles IME, and owns Tab focus.

- `win32_native_settings.cb` - the settings panel on real Win32 controls.
- `cocoa_native_settings.cb` - the same panel on real AppKit controls (macOS).

## 05-gallery

Every element in one app, and the chapter that proves host-neutrality.

- `gallery_app.cb` - the `GalleryApp` component and its headless self-test. It imports
  only the element model and has no `main()`; the host free functions it calls are
  supplied by whichever launcher imports it.
- `gallery.cb` - the Win32/Cocoa launcher, plus the screenshot path.

The WinUI launcher for this same component lives in chapter 06.

## 06-winui

A third backend, on real XAML controls, from an unpackaged self-contained exe.

- `winui_app_demo.cb` - the raw bring-up: bootstrapper, `Application.Start`, a projected
  `Click` handler. No framework involved.
- `winui_demo.cb` - an app-facing `Component` on the WinUI `NativeHost` backend.
- `winui_gallery.cb` - chapter 05's `gallery_app.cb`, unmodified, on the WinUI host.

These need the pinned Windows App SDK NuGet packages, which the sources pull in
themselves via `import package-nuget`.

## 07-testing

The copy-me template for testing a `ui_native` app. Copy both files, rename, and swap
in your own component.

- `todo_app.cb` - the app, host-neutral and with no `main()` (like `gallery_app.cb`).
- `todo_test.cb` - the test target: imports a host, the `ui_test.cb` framework, and the app.

Note the discipline the framework depends on: every element the test drives gets an
explicit key, so the test addresses it by a stable path instead of a positional index.

```bash
x64/Release/cflat.exe example/ui/07-testing/todo_test.cb -i example/ui/07-testing -o out/todo_test.exe
out\todo_test.exe --list
```

## 08-fedit

The flagship, and the "not Electron" pitch: a self-contained native exe with real OS
controls, a real menu bar and accelerators, real shell file dialogs, and a real
multiline editor - no browser engine.

- `fedit.cb` - a small IDE shell: file-tree sidebar, multi-tab documents, a draggable
  splitter, a context menu, and a toolbar.

```bash
x64/Release/cflat.exe example/ui/08-fedit/fedit.cb -i example/ui -o out/fedit.exe
out\fedit.exe
```
