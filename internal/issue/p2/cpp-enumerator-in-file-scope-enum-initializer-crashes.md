# File-scope enum initializer naming an imported C++ enumerator crashes (CreateLoad, no insertion block)

`enum Fold : int { Value = (int)r.U.X };` with `r.U.X` an UNSCOPED imported C++ enumerator crashes (rc 139,
internal compiler error) on master. Since T40 (scoped enumerators registered only as `E.v`), the qualified scoped
spelling `(int)r.E.A` takes the same path and crashes too (master refused it with "enum value must be a fixed
integer value"). Found by the T40 Sol final review (2026-10-05).
Probes: scratch/repro_keep/t40_sol3/folding/ (unscopedinit.cb, enuminit.cb, h.h; *_M = master, *_B = branch).

## Fix direction

The enum-initializer folder evaluates the operand dynamically at file scope (no insertion block). Fold an imported
C++ enumerator (scoped or unscoped, qualified) from its registered constant value, as CFlat enumerators fold; any
operand that still is not a compile-time constant gets the existing "fixed integer value" LogError, never a load.
Pairs with p3/cpp-scoped-enumerator-case-label-if-const-alias (same constant-folding gap in case labels / if const).
