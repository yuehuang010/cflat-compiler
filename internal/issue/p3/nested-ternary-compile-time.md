# Nested `?:` compile time grows about 2x per level

Found by TERNADDR (2026-10-31) while measuring the nesting-depth crash; present on master, not caused by TERNADDR.

## Repro

`int* r = (i == 1 ? p : (i == 2 ? p : ... (i == k ? p : q)...));` in an initializer, master exe:
k = 24: 2 s; k = 28: 26 s; k = 30: 96 s; k = 32: over 120 s. The right-nested form (`i == k ? p : i == k+1 ? ...`)
is also over 60 s at k = 40.

## Root cause

Not diagnosed. Each `?:` level appears to typecheck or lower its arms more than once, so the cost multiplies.

## Fix direction

Find the repeated arm evaluation in ParseTernaryBranches (count ParseConditionalExpression calls per level with
-v or a counter) and make each arm parse once. The TERNADDR stack-headroom check (RequireStackHeadroom) only bounds crashes, not time: pointer-arm chains
stay slow up to the point the stack check refuses them.

## Residue (TERNADDR, pre-existing on master, not guarded by the stack check)

Found by the TERNADDR review 2 probes; master and the TERNADDR fix both crash (139) on the first three.
- Unguarded early-pass walkers:
  - paren `?:` 200 levels: crash in CFlatRuleContext::AppendText (getText).
  - else-if 3000 levels: crash in PrepareMovedReassignments.
  - `((x)+1)` 400 levels: crash in ScanGenericTypeUses.
  - paren-only input (about 459 parens, master 458) overflows in the ForwardRefScanner walkers.
- 100000 nested parens: time out (over 150 s) on both.
- ANTLR parse overflows near 6000 parens; there is no pre-parse guard.
