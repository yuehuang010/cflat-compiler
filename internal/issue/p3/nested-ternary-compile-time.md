# Nested `?:` residue (TERNADDR, pre-existing)

The 2x-per-level compile time is fixed (b9f39cdb, per-query const-walk memo); its section was deleted 2026-11-07.

## Residue (TERNADDR, pre-existing on master, not guarded by the stack check)

Found by the TERNADDR review 2 probes; master and the TERNADDR fix both crash (139) on the first three.
- Unguarded early-pass walkers:
  - paren `?:` 200 levels: crash in CFlatRuleContext::AppendText (getText).
  - else-if 3000 levels: crash in PrepareMovedReassignments.
  - `((x)+1)` 400 levels: crash in ScanGenericTypeUses.
  - paren-only input (about 459 parens, master 458) overflows in the ForwardRefScanner walkers.
- 100000 nested parens: time out (over 150 s) on both.
- ANTLR parse overflows near 6000 parens; there is no pre-parse guard.
