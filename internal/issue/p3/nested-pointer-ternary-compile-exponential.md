# A nested pointer ternary compiles in time exponential in its depth

Summary: `c ? (c ? (... ) : p) : p` style nested pointer ternaries take about 2x longer per extra level to compile
(n=20/22/24: 0.18/0.54/1.93 s on the master exe, `--check`). At ~41 levels a compile with a large stack (Windows 64 MB,
or Mac with `ulimit -s 65520`) runs for minutes instead of finishing or being refused.
Found: TERNADDR r4 (box 7 run 2, 2026-10-31), PRE-EXISTING (master exe shows the same curve). err_ternary_nesting_depth
block 1 only refuses because the guard runs out of stack on the first descent; its depth is sized so that holds on every host.
Fix direction: find the arm walk that re-visits each nested arm (likely a per-arm re-parse or classification pass in
ParseTernaryBranches / ParseTernaryBranchesBody) and memoize or make it single-pass; acceptance: n=40 compiles in < 1 s.
