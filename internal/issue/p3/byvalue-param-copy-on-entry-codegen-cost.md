# P3: by-value owning param copy-on-entry analysis costs ~7% CodeGen on test_collection_leaks

Found 2026-09-30 at the W7 landing (by-value owning param leftovers after B13, now in squashed
commit 49044ba8 "Native ownership and lifetime rules", was a13c4430). Accepted by the maintainer
2026-09-30 as a known cost; this item exists so a future profile run can narrow it down.

## Measurement
- `Test/test_collection_leaks.cb`, cold `-B` wall, fresh `CFLAT_CACHE_DIR`: W7 round 5 was +13%
  vs master (1.39 s vs 1.20 s), all of it in CodeGeneration (`-ftime-trace`).
- getText prechecks added before landing brought it to ~+7%. test_libs totals did not move
  (cold 97-120 s across the day's gates, noise-bound).

## Where
The copy-on-entry decision ("copy only for real writes: whole stores, alias deref stores, container
mutators reached by any path") runs per-function walks over each body that has a by-value owning
parameter. Cost scales with function count x body size, so heavy generic/collection files pay most.

## Direction
- Profile first (Instruments / `-ftime-trace` with finer scopes around the W7 walks) - do not guess.
- Likely wins: one walk per function shared by all params instead of one per param; cache the
  write-set per function body; skip functions whose params are never named in a write position
  (cheap token precheck already covers part of this).
- Acceptance: test_collection_leaks CodeGen back within ~2% of the pre-W7 master, suite unchanged.
