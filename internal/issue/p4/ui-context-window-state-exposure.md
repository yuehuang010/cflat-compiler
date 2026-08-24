# Proposal: expose requested window state (size/topmost) on IUiContext

## Proposed surface
`UiContext.setContentSize`/`setTopmost` record `requestedWidth`/`requestedHeight`/
`requestedTopmost` (cflat/core/ui_native.cb ~557-559) for canvas/TUI/headless hosts with no
native window, but those fields are not on `IUiContext`, so an interface-typed caller - the
case `ui-native-widget-interfaces-omit-settable-fields` was filed about - cannot read the
model state back. Proposal: add read accessors (e.g. `int requestedWidth()`,
`int requestedHeight()`, `bool requestedTopmost()`) to `IUiContext`, implemented by all
hosts (native hosts return the live window state).

## Alternatives
- Leave unexposed and document setContentSize/setTopmost as fire-and-forget on headless
  hosts (status quo; the acceptance criterion of the deleted
  `p3/ui-native-window-level-api-gaps.md` asked for an observable path, which timers got
  but these did not).
- Expose only via the ui_test driver surface instead of the app-facing interface.

## Acceptance
Maintainer ruling needed on whether the app-facing interface grows read accessors, before
any implementation.

Found by code review 2026-08-24 (Opus bulk review of 708dfe3..9470211).
