# auto local of an arithmetic expression deduces CFlat's width type at C++ template calls

Integer half: Fixed in `cflat/MainListener_Declarations.cpp:MainListener::ParseDeclaration(DeclarationSpecifiersContext*, InitDeclaratorListContext*, ...)`.
Floating half remains open. Regression coverage: `Test/test_cpp_interop.cb` T58 leg 27070, classified by `Test/library/cpp_t27_ptrdiff.hpp::integer_arith_kind`.

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

Pre-existing, seen in the autoloc review (2026-11-01, master 871a238f): a direct native call `nf(l + 1)` (`long l`, overloads
`nf(long)` + `nf(long long)`) picks `nf(long long)`, clang `nf(long)`; after autoloc `auto x = l + 1; nf(x)` picks `nf(long)` (main-session
decision: accepted - `auto` is the expression's type and `long` is a C spelling), so the direct call is now the odd one out. Repro
scratch/repro_keep/autoloc/rev_autoloc_nat_direct.cb (main checkout).
