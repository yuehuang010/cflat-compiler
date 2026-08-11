# The deferred end-of-body checks never run for LAMBDA bodies

Re-filed 2026-08-10 out of the now-deleted `p1/unique-field-to-field-interface-receiver-residues.md`
(shape 5), which was itself closed by the 2026-08-10 uniform-implicit-move ruling. What that ruling does NOT close is the
architectural gap underneath shape 5: the deferred end-of-body checks are called only from the
NAMED-function completion path, so a lambda body never reaches them.

## Summary

Four deferred checks shared one hook in `cflat/MainListener_Declarations.cpp` (the named-function
completion path, beside `EndAutoReturnCapture`). Re-read on `fix/uniq-implicit-move`, they do NOT
all suffer equally - the residues file listed all four, but only two are actually unprotected:

| Check | Lambda body today |
|---|---|
| `RunUniqueIfaceFieldStoreCheck` | DELETED by the 2026-08-10 uniform-implicit-move ruling |
| `RunNullDerefDataflow` | COVERED - `RunMoveDataflow` sweeps every non-empty function at module end, explicitly "lambdas and synthesized bodies do not go through the named-function completion point" |
| `RunInterfaceReturnDangleCheck` | NOT RUN - `pendingReturnDangleChecks_` is cleared unanalyzed at module end |
| `RunNullIfaceDispatchCheck` | NOT RUN - `pendingNullIfaceDispatch_` is cleared unanalyzed at module end |

So the live gap is the last two. Both drop their records deliberately at module end (accept, never
reject), with the reason stated in `LLVMBackend_MoveDataflow.cpp::RunMoveDataflow`: by module end
`interfaceBoxRecords_` no longer describes that function, so the ledger lookups the checks depend on
would answer for the WRONG function. That is a correct conservative bail, not an oversight - but it
means a flagged store or dispatch inside a lambda is silently never diagnosed.

Severity P2: missing diagnostic only, never a false rejection. Both checks guard real double-free /
null-dispatch shapes, so the same program written as a named function is rejected and written as a
lambda is not - an inconsistency a user hits by refactoring.

## Repro

The pre-ruling witness was the interface field-to-field store, which no longer diagnoses at all.
A replacement witness must be built from one of the two live checks - take an existing rejecting
body from `Test/errors/` that fires `RunInterfaceReturnDangleCheck` or `RunNullIfaceDispatchCheck`,
and rewrite it as a lambda body. Confirm the named spelling still rejects on the binary under test
first: the issue is the DIFFERENCE between the two spellings, not the absolute verdict.

## Root cause

The end-of-body drain is written once, in the named-function completion path of
`MainListener_Declarations.cpp`. A lambda body's `llvm::Function` is completed elsewhere and never
calls it. The records are correctly per-`llvm::Function`, so nothing leaks into the enclosing
function - they are just never drained, and are then cleared unanalyzed.

## Fix direction

ONE shared hook fixes both at once: factor the end-of-body calls into a single
`RunDeferredEndOfBodyChecks(llvm::Function*)` on `LLVMBackend` and call it from the lambda-body
completion path as well as the named-function one.

Two things the existing call site already depends on and that the new call site must preserve:

- ORDERING: the checks read the finished CFG and the slots' complete use-lists, so the hook must run
  after the body's last instruction is emitted and before the builder returns to the enclosing
  function - which is also the only point where `interfaceBoxRecords_` still describes this body,
  the exact fact the module-end bail was avoiding.
- THE ABORT PATH: `DiscardPendingNullIfaceDispatch` must be reached for an aborted lambda body too,
  or a partial CFG is walked.

Polarity is unchanged and must stay prove-then-reject: anything still unprovable keeps compiling.

Related: [[interface-issue-queue]]
