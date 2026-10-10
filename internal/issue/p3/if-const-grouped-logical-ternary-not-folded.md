# Grouped logical / ternary `if const` conditions are refused where clang folds them

Found by the T45 round-4 Sol review (2026-10-05). PRE-EXISTING on master.

## Repro (scratch/repro_keep/t45_r4/entry_and_group.cb, entry_join_dead.cb + .cpp twins)

A parenthesized `&&` / `||` / `?:` group inside an `if const` condition (pure operands, or an
untaken assignment arm) is refused as not constant. clang++ -std=c++20 accepts and prints 11.
Top-level (ungrouped) forms already fold.

## Root cause

Leaf emission of a grouped logical/ternary produces scratch loads and PHIs across block joins;
TryFoldConstInt folds Select but not those loads/PHIs.

## Fix direction

Lower constant-context logical/ternary groups eagerly (Select, as enum initializers do after
T45), or teach TryFoldConstInt to fold a PHI whose incoming values and branch conditions fold.
Keep the side-effect rejection for taken assignment arms.
