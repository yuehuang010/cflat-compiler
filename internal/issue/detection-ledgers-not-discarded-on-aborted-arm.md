# Detection-only ledgers are not discarded when a '?:' arm aborts

Filed 2026-07-25. Surfaced by review of commit 52c64cb ("Reject a move-of-borrowed '?:'
join stored into a unique local or field"). Pre-existing design property; the new
movedBorrowedPtrValues_ ledger inherits it exactly, it did not worsen it.

## Status

KNOWN / OPEN. Theoretical - no reproducing case has been constructed. NOT a memory leak
(the ledgers hold non-owning llvm::Value* keys; the values belong to the LLVM module).
Hygiene gap: a design rule exists only in review transcripts, not in code.

## Symptom (hypothetical)

When a `?:` arm's lowering throws a compile error, the catch block in the ternary
lowering (`cflat/MainListener.h`, ~11024-11040) calls `DiscardOwnedTempsSince(mark)`
to drop what the aborted arms registered. That helper (`cflat/LLVMBackend.h`, ~2801)
trims only the four `pendingOwned*` vectors. The four detection-only ledgers -
`ownedReturnTemps_`, `ownedNewTemps_`, `movedOutPtrValues_`, `movedBorrowedPtrValues_`
(and `valueElementTypeNames_`) - keep whatever the aborted arm registered.

If a later consult ever matched a stale key against a recycled llvm::Value* address:

- stale `movedOutPtrValues_` entry -> wrong ADOPTION (double free at runtime);
- stale `movedBorrowedPtrValues_` entry -> spurious "cannot assign/store borrowed"
  compile error with no workaround.

## Why it has never fired

Every condition must line up, and the last one has no known construction:

1. The throw must be caught and compilation continue - only `expect_error` compiles
   (LSP reanalysis clears both ledgers in `ResetForReanalysis`).
2. The stale entry survives exactly ONE statement: the next full expression's
   `FlushOwnedTemps` (`cflat/LLVMBackend.h`, ~2814-2826) clears all ledgers.
3. Within that window, a new RHS value must occupy the SAME llvm::Value* address as an
   aborted-arm value. The aborted arm's blocks are terminated and kept in the module,
   not deleted, so their instruction addresses are not recycled. Two independent review
   attempts failed to construct a collision.

Even on a hit, the compile has already diagnosed an error, so no binary is produced;
the harm is one wrong or extra diagnostic inside an already-failing compile.

## Root cause

`DiscardOwnedTempsSince` predates the detection-only ledgers and was never extended.
The pendingOwned* vectors need mark-based trimming because entries before the mark
carry live free obligations; the detection-only ledgers are expression-scoped facts,
so an aborted expression invalidates them wholesale.

## Fix direction

In `DiscardOwnedTempsSince`, also clear the five detection-only containers outright
(`ownedReturnTemps_`, `ownedNewTemps_`, `valueElementTypeNames_`, `movedOutPtrValues_`,
`movedBorrowedPtrValues_`) - no mark needed, wholesale clearing is always safe for
facts about an expression that no longer exists. Alternative (only if a case is ever
found where pre-arm facts must survive the abort): extend `OwnedTempMark` with their
sizes and trim like the others. Either way, state the rule in the function comment:
"detection-only ledgers must be discarded on an aborted region" - so the next ledger
added picks it up. No test is possible without a repro; the change is verified by the
existing suite staying green (`./test.sh Release`, examples).
