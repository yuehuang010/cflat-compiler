# Deep nesting residue after the walker stack guards

The 2x-per-level `?:` compile time is fixed (b9f39cdb). The early-pass walker crashes are fixed (fix box
2026-11-07): every recursive pre-pass walker checks stack headroom and refuses with "expression or
statement is nested too deeply; split it into named temporaries" (RequireStackHeadroom), or was
rewritten as a loop (AppendText, scanFunction). Nothing master compiled is refused.

## Residue (pre-existing on master, which crashes at these sizes too)

- Nested parens near 950 levels still exit 139 (seen 2026-11-07 on the Mac Release build; 450-750
  are refused cleanly). Not traced; the implementer saw the crash inside ANTLR's
  SingletonPredictionContext shared_ptr release (recursive teardown of the prediction-context chain
  when the parse is destroyed), and earlier ?: / `((x)+1)` runs showed the same frames near the stack
  limit.
- 100000 nested parens: time out (over 150 s).
- ANTLR's own recursive-descent parse overflows near 6000 parens; there is no pre-parse guard.

Fix direction: a pre-parse token scan that counts bracket / `?` nesting and refuses above a bound
that ANTLR's parse and teardown survive on the 8 MB macOS main thread (measure per shape), or run the
parse on a thread with a larger stack.
