# Native UI has no timer or periodic UI-thread tick

Imported issue #22 from winmempress's `COMPILER-ISSUES.md`. Triage: p2.

## Repro / use case

Implement a polling refresh, caret blink, animation, debounce, or periodic data update
in a native UI app. The current API offers `IUiContext.post` for work initiated by a
thread, but no way to schedule work after a delay or at a repeating interval. Starting
a worker thread and posting forever introduces teardown races and unnecessary lifetime
complexity for a UI-only task.

## Verified current-code evidence and root cause

`cflat/core/ui_native.cb` defines `IUiContext.post`, backed by a boxed `PostBox`,
`bindPost`, and host message marshaling. There is no timer type, timer handle,
`SetTimer`, `NSTimer`, or periodic dispatcher operation in the native hosts. The current
plan in `internal/plan/ui-map-canvas.md` independently identifies the missing seam as
`hostStartTimer`/`hostCancelTimer` with a boxed closure, confirming this is an API gap
rather than a missing app helper.

## Scope

The timer must run callbacks on the UI thread and cover Win32, Cocoa, and WinUI: the
first two have their native timer mechanisms, while WinUI needs its dispatcher timer.
The `ICanvas`/TUI/headless hosts have no wall-clock native loop in every test mode, so
they need a documented deterministic pump/test behavior rather than silently spawning
a thread. The report's two-host assumption is incomplete for this seam.

## Bounded fix direction / acceptance criteria

- Reconcile the plan's host start/cancel seam with an app-facing API such as a context
  timer/every operation; settle the exact spelling and handle type before implementation.
  Support one-shot and repeating schedules, cancellation, and a defined callback
  ownership/lifetime model.
- Box closures like `PostBox`, route callbacks on the UI thread in Win32, Cocoa, and
  WinUI, and cancel all outstanding timers during window/host teardown. A callback must
  not run after cancellation or retain a destroyed app/window.
- Provide deterministic headless/canvas driving or a documented no-op mode and extend
  an existing UI test for one-shot, repeat, cancellation, UI-thread affinity, and
  leak-clean teardown. Avoid requiring a worker thread for ordinary periodic UI work.
