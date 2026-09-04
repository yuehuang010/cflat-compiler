# q04 - BATCH: one-site lowering and diagnostic fixes

Ships as ONE worktree (`fix/batch-lowering`), one commit, one scoped review round, per the
fix-issue skill's batch mode. Four members, all pre-existing, all with the site and the intended
lowering already in the file, none adding a rejection. Expected cost: one fix-agent run, one
review, two rebuilds.

## Members

| # | Item | Site | Leg |
|---|------|------|-----|
| 1 | `p2/float-not-equal-is-ordered-nan-compares-equal` | float `!=` lowering, `CreateFCmpONE` -> `UNE` (LLVMBackend_VariablesAndIR.cpp ~1695); check simd float `!=` and constant folding | value legs `n != n`, `n != 1.0`, `1.0 != 2.0`, `1.0 != 1.0` + `--symbol-dump-ir` shows `fcmp une`; restore `pv != pv` in Test/test_core.cb's poison leg |
| 2 | `p3/bool-cast-truncates-instead-of-testing-nonzero` | integer/float/pointer -> bool cast, `CreateTrunc` -> `icmp ne 0` / `fcmp une 0.0` / `icmp ne null` in `CreateCast`; also `bool b = 2;` | `(bool)2`, `(bool)256`, `(bool)v`, `bool b = 2`, `(bool)0.5`, `(bool)(int*)nullptr` in Test/test_basic.cb; then drop the `(T)3 == (T)1` test in core/array.cb for a real predicate |
| 3 | `p3/lost-count-return-diagnostic-doubles-the-brackets` | the "element count is lost at the return" LogError site: spell the ELEMENT type, reword the subject | err leg on the corrected substring in the existing err file for this rule (`--locale pseudo`) |
| 4 | `ui/symbol-dump-opt-ignores-sanitize-flag` | thread `--asan` / `--sanitize=ownership` from ArgParser into the symbol-view compile options as `-O` already is | dump of a `__SANITIZE_OWNERSHIP__`-gated function shows the memset with the flag, not without |

## Constraints

- Member 1 and 2 are in the same file, different functions; 3 and 4 are elsewhere. Disjoint.
- Message change in 3 regenerates `en-pseudo.json`; commit the generated diff as-is.
- If any member turns out not to be one-site, drop it from the batch and say so; do not grow
  the batch into a full-mode fix.
