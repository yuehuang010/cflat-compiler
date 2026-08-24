# Native UI leaf elements cannot participate in flex layout

Imported issue #20 from winmempress's `COMPILER-ISSUES.md`. Triage: p2.

## Repro / use case

Build the ordinary desktop layout of a flex column: a fixed toolbar, a list/editor that
should consume remaining height, and a fixed status bar. Assign a positive flex value to
the `ListView` (or another leaf). The parent distributes space only to flexible `View`
children; the leaf keeps its intrinsic `height`, so the list does not fill the available
area. A wrapper does not transfer the parent's allocated share to the leaf, so the layout
cannot express this common arrangement without manual size arithmetic.

## Verified current-code evidence and root cause

`cflat/core/ui_native.cb` puts `flexWeight()` on `IElement`. `View.flexWeight()` returns
`this.style.flex`, but the other element implementations, including `ListView`, return
`0`. `View.isRowFlexChild` and `isColumnFlexChild` therefore exclude every leaf from
the share calculation. The wrapper workaround is not equivalent: a `View` is
content-driven by contract, so wrapping a leaf does not make the leaf consume the
parent's leftover height.

There is a second half to the defect. Even if a leaf reports a positive flex weight,
its `layout()` must honor the share passed in `LayoutConstraints`. `ListView.layout`
clamps width against `c.availW` but takes height from its own `height`; similar leaf
implementations need an explicit axis policy. Adding a `flex` field alone would leave
the computed share unused.

## Scope

This is shared layout behavior consumed by Win32, Cocoa, and WinUI native hosts and by
the `ICanvas`/TUI/headless paths, which all position or paint from `nodeBounds()`. The
design must define bounded-axis behavior, intrinsic sizing when an axis is unbounded,
explicit width/height precedence, padding/gaps, and minimum sizes without making each
host invent a different rule.

## Bounded fix direction / acceptance criteria

- Decide and document the leaf flex contract, including the field/interface exposure and
  which axes consume a parent share. Handle both halves together: `flexWeight()` must
  report the configured value, and every participating leaf `layout()` must use the
  supplied share while honoring explicit dimensions and minimums.
- Preserve intrinsic sizing for non-flex leaves and for unbounded scroll axes. Ensure
  row and column distributions, gaps, padding, and remainder cells still sum without
  overlap or drift.
- Extend an existing layout/UI self-test with at least a flexing native leaf and a
  flexing canvas/headless leaf, checking `nativeNodeBounds`/model bounds after resize.
  Verify all three native hosts consume the same resulting bounds.
