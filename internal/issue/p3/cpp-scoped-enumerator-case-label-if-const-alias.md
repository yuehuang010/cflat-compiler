# Qualified imported C++ scoped enumerators fail in case labels, if const, and through an enum type alias

`case cppi_scope.Hue.Red:`, `if const (cppi_scope.Hue.Red == ...)` and `using H = cppi_scope.Hue; H.Red` fail on
master (constant folding / alias member lookup). Since T40 refuses BARE scoped enumerators (C++ rule), the qualified
spelling is the only one left, so these gaps matter more. Found by T40 (2026-10-05); cells in worktree
cflat-fix-t40-enumclass scratch/t40_matrix.md.

## Fix direction

Constant-fold qualified C++ enumerators wherever CFlat enumerators fold (case labels, if const, array sizes); resolve
`Alias.Member` through a `using` alias of an imported enum type.
