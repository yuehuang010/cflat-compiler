# q07 - Investigation: `--sanitize=ownership --run` teardown flake

One member, full mode, and the only open item without a root cause. Not batchable: the fix is
unknown until the crash is reproduced deterministically.

| # | Item | Status | Shape |
|---|------|--------|-------|
| 1 | `p2/sanitize-ownership-run-teardown-flake` | INVESTIGATE | 2 of 12 runs hang / SIGSEGV / exit 1 after `main` returns; AOT and plain `--run` clean. Build Debug (assertions-on LLVM), run under guard malloc / `MallocScribble`, get a deterministic abort and a backtrace, then move the sanitizer's exit-time work ahead of JIT teardown or skip it under `--run`. |

Constraints: the program is legal - do not widen the `--run` rejection list. Distinct from the
HeapAudit JIT module gap (ruled no-fix 2026-09-03).
