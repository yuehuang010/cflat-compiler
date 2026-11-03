# auto local of an arithmetic expression deduces CFlat's width type at C++ template calls

Pre-existing (master same); found by the T58 review 5 (2026-10-06).
- `auto x = l + 1; vv.fwd(x)` deduces long long (clang long).
- `auto x = 1e3 + 1; vv.byv(x)` deduces float (clang double).
- `auto x = 1e3 + 1; vv.fwd(x)` refused: "no overload ... matches [0] float".
T58 carries the C++ arithmetic identity (CxxArithIdentity) on rvalue expressions only; an `auto`
local is typed by CFlat's width rule and loses it. Probes: scratch/repro_keep/t58_rev/rev5/.

Fix direction: decide whether `auto` from C++-identity arithmetic keeps that identity (variable
type = the C++ type, e.g. double / long) - clang oracle per cell; native CFlat `auto` unchanged.

RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31), integer half: an `auto` local KEEPS the C++ arithmetic identity
of its initializer - `auto x = l + 1;` is `long` (target long), not `long long`, because C spellings are aliases and primitive identity
matches C++ (ruling 2026-09-10; L-suffix = target long 2026-09-30); `auto` is the expression's type, nothing more. Native CFlat `auto`
over CFlat-spelled operands is unchanged. The floating half (`auto x = 1e3 + 1;` float vs double) is the literal-typing question
DEFERRED 2026-10-13 and is not ruled here. Test consequence: `vv.fwd(x)` with `auto x = l + 1` deduces `long` (accept leg); no leg flips.
