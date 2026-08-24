# Native UI lacks common window-level controls and queries

Imported issue #21 from winmempress's `COMPILER-ISSUES.md`. Triage: p3.

## Repro / use case

An application needs to react to the current client size, request a content-size
change, keep a utility window above its owner, or query the operating system's dark-mode
preference. The framework exposes `hostWidth()`, `hostHeight()`, and `hostDark()`, but
does not expose the corresponding general window operations. A fork consequently grew
a private `ui/platform.cb` mini-host for these calls instead of using the shared API.

## Verified current-code evidence and root cause

`cflat/core/ui_native/win32.cb`, `cocoa.cb`, and `winui.cb` each expose free functions
`hostWidth()` and `hostHeight()`; the native hosts also expose `hostDark()` as the last
theme applied by the framework. There are no shared public equivalents for
`setContentSize`, `setTopmost`, or `systemPrefersDark`.

The backends already contain implementation seams: Cocoa calls `setContentSize:` in
`nativeResizeClient`, Win32 has top-level `SetWindowPos` and close/window state code,
and WinUI caches the HWND through `IWindowNative` and applies title-bar theme through
`winuiApplyTitleBarTheme`. Those are internal/test paths, not a stable app API.
`nativeResizeClient` is a driver (Win32 synthesizes `WM_SIZE`; it does not resize the
HWND), so it does not close this gap. Likewise `hostDark()` reports applied framework
theme, not an OS preference query.

## Scope

Design one bounded window-level API across Win32, Cocoa, and WinUI, with explicit
behavior for multi-window current-context selection. Canvas/TUI/headless hosts have no
real top-level window, so each new operation needs documented no-op, model-state, or
unsupported behavior and a deterministic test path. Do not expose raw HWND, NSWindow,
or WinRT objects.

## Bounded fix direction / acceptance criteria

- Inventory and specify the smallest useful set first (content-size request, topmost
  state, and a separate OS-preference query are plausible candidates). Keep
  `hostDark()` semantics unchanged unless the API deliberately adds a distinct
  preference name and documents the difference.
- Implement the chosen calls in all three native hosts, routing to the active/current
  window and handling headless/canvas behavior explicitly. Keep existing native test
  drivers separate from app-facing operations.
- Add an existing host-neutral/window self-test for return values and state transitions,
  plus native checks where the OS can observe them. Verify multi-window targeting and
  teardown do not retain stale window handles.
