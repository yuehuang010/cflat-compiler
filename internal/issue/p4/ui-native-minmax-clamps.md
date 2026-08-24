# Proposal: per-axis min/max clamps on Style, applied after flex distribution

## Proposed surface
Add `minWidth`/`maxWidth`/`minHeight`/`maxHeight` to `Style` (cells, 0 = unset), applied as
the LAST step of each leaf/container layout: after explicit dimensions and exact-flex axes
are resolved, clamp the bound into [min, max] per axis. This mirrors the consensus model
(CSS flexbox, Flutter BoxConstraints, WPF Min/Max*) where clamps win over flex and over
explicit sizes, and it solves "a flexed sibling crushes this control to zero" without a new
layout pass.

## Design doctrine (ratified direction, 2026-08-24)
cflat ui_native follows the standard constraints-down/sizes-up model (Flutter/WPF-shaped):
availW/exactW flow down, bounds come up, flex weights distribute the remainder, explicit
style dimensions win over flex (maintainer ruling 2026-08-24). Do not invent novel layout
semantics; when a gap appears, adopt the established equivalent (clamps, baseline
alignment, etc.) rather than a bespoke rule.

## Alternatives
- Do nothing: apps hand-tune flex weights to avoid crushing; fragile across window sizes.
- Full CSS semantics (explicit width as flex basis, grow/shrink both ways): rejected -
  contradicts the ratified explicit-wins rule and adds shrink accounting for little gain.

## Acceptance
Maintainer ratifies field names and the clamp-wins-over-everything precedence; then
implement in ui_native.cb layout passes (all leaves + containers), document in doc/UI.md
next to the flex rules, and extend the gallery layout self-test in place.
