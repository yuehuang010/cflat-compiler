# auto local of an arithmetic expression deduces CFlat's width type at C++ template calls

Pre-existing (master same); found by the T58 review 5 (2026-10-06).
- `auto x = l + 1; vv.fwd(x)` deduces long long (clang long).
- `auto x = 1e3 + 1; vv.byv(x)` deduces float (clang double).
- `auto x = 1e3 + 1; vv.fwd(x)` refused: "no overload ... matches [0] float".
T58 carries the C++ arithmetic identity (CxxArithIdentity) on rvalue expressions only; an `auto`
local is typed by CFlat's width rule and loses it. Probes: scratch/repro_keep/t58_rev/rev5/.

Fix direction: decide whether `auto` from C++-identity arithmetic keeps that identity (variable
type = the C++ type, e.g. double / long) - clang oracle per cell; native CFlat `auto` unchanged.
