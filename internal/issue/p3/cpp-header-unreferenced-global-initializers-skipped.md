# C++ header global initializer residue after HDRGLOB

HDRGLOB (d13af01d8, ruled 2026-10-25 follow clang) landed: every harvested header global's dynamic initializer runs once at
startup, owned by the first source-ordered import of its header, in import order. Residue (repros
scratch/repro_keep/hdrglob/):

- `if const` import order: IFCORDER (f2cbf5171) discovers an arm whose condition names a const from an imported .cb at
  its source position. Still late (init order 234 vs clang 324, values right): a condition naming a ROOT const declared
  earlier in the same file (kept late on purpose - folding it before its own imports would let a same-named imported
  const win, repro K12), and a namespace-qualified imported const `if const (A.K == 1)` (repro P13).
- A non-owner header global that needs destruction keeps clang's own emission instead of being stripped (stripping
  crashed the MS ABI atexit stub): inline ones are guard-merged and run once; a non-inline external one shared by two
  import groups is an ODR violation that fails to link (master too).
- Pre-existing: the same include-guard macro used by two different header files is not deduplicated.
