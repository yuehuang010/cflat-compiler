# Registering harvested C++ signatures projects every record they name

Found in perf timebox 2026-09-29d (runs D4, D9).

## Symptom
Torch: CHeaderRegister ~350 ms cold on every case, RegisterCSignatures 308 ms of it (~10.5k signatures).
`sample`: RegisterCSignatures -> CreateFunctionDeclaration -> BuildAbiRecipeFromClangPlan -> GetType ->
EnsureCxxRecordProjected -> CompleteCxxRecordSpecialMembers -> RunCxxTypeRequests (61 of 130 samples in the
completion requests). clang++ does no equivalent work for functions the program never calls.

## What was tried
Branch perf/d4-lazy-cxx-decls: 453c18ea skips building the declaration (and so the projection) at registration:
torch -3.6% cold instructions, but projection is also what registers member operators into the overload tables, so
candidate order / tie-breaks / candidate lists / postfix free operator++ / macro aliases / --symbol changed (review
P1s). The branch tip (Opus rework) keeps registration + projection as master and defers only the llvm::Function:
parity restored, torch gain 0.

## Fix direction
Keep projection at registration (order), defer what it triggers: special-member completion (ruling R1: on first
use) - run D9 in the same timebox - and anything else the recipe does not need. Or register member operators from
the harvest directly so projection can move to first use.
