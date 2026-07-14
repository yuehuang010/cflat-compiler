# Intermittent "Internal compiler error during analysis" in the LSP bulk sweep

Created: 2026-07-13 (hit while running test_lsp as a gate for the Win32 UI work)

## Summary

`test_lsp.bat`'s bulk sweep intermittently fails one file with
`Internal compiler error during analysis` on `example/ui/01-elements/app.cb`.
Observed ONCE as `205 passed, 1 failed`; the same suite passed `206 passed, 0 failed`
on roughly 9 of 10 subsequent runs, including several full clean runs. Rough
reproduction rate: ~10% of bulk runs.

A `.cb` SOURCE file should never be able to provoke an internal compiler error, so
this is a compiler/LSP bug, not a UI or example bug. It was NOT introduced by the
Win32 UI work (it appears and disappears independently of those edits), but that has
not been proven against a clean baseline build.

## Repro

Not reliably reproducible on demand. `scratch/lsp_focus.py` (written for this, keep
it if the scratch dir is cleaned) drives the LSP with the analysis pool at size 1 -
so every file re-analyzes through the SAME backend, the state-leak-sensitive
configuration the L1 regression test also forces - and cycles a small file set
(`core/ui_native/win32.cb`, `core/ui_native/host.cb`, `example/ui/01-elements/app.cb`,
`example/ui/05-gallery/gallery_app.cb`), reporting how many iterations produced the
crash diagnostic. A focused run at pool=1 came back 9/9 clean, so the trigger is
probably an ordering/state condition the bulk sweep hits, not that file alone.

    python scratch/lsp_focus.py <exe> <iterations>

## Root cause

Unknown. Prime suspect is leaked per-call state across re-analysis: per CLAUDE.md,
`ResetForReanalysis` must clear ALL transient per-call state (the `lastCallIsBonded`
precedent) or a value left set by an ABORTED compile leaks into the next file's
analysis. An intermittent, order-dependent internal error in a bulk sweep is exactly
the shape that produces.

## Fix direction

Audit `ResetForReanalysis` against every transient field added since it was last
reviewed - especially anything set on an error path that returns early, since the
failure only shows up when a prior file's analysis aborted. Then make the LSP surface
the actual assert/exception text instead of the generic "Internal compiler error
during analysis" wrapper, so the next occurrence names the failing invariant.
