# A temp's unique field laundered through a borrowing callee into a local reads freed memory

Filed 2026-08-10 by the `fix/joinlife` review. P1: a USE-AFTER-FREE that compiles and reads
freed memory. Pre-existing - measured identical on `a48829b` (`/tmp/jlpre/cflat`) and on
`fix/joinlife` (`dd7b5b5`), so it is NOT a regression of that fix. Unfiled until now; it was
noted in the `fix/joinlife` digest entry as "a different, pre-existing hole".

## What

`RejectOwningTempUniqueFieldEscape` rejects storing a temporary's `unique` field into a local:

```cflat
Resource* b = makeMoveBox().t;   // REJECTED, correctly
// rv6.cb(7,34): cannot bind 'MoveBox__unique_Resourceptr.t' taken from a temporary to a
// local; its buffer is owned elsewhere and would be freed out from under the local.
```

Routing the same field through a callee that only BORROWS its parameter defeats that guard: the
value the declaration sees is a plain `CallInst` result, not a ledgered temp field, so nothing
fires. The temp is still destructed at the statement boundary, so the local holds a dangling
pointer the moment the statement ends.

## Repro (`scratch/rv/rv4.cb`)

```cflat
int dtorCount = 0;
struct Resource { int id = 0; ~Resource() { dtorCount = dtorCount + 1; } };
struct MoveBox<T> { T t = default; };
MoveBox<unique Resource*> makeMoveBox() { MoveBox<unique Resource*> b = default; b.t = new Resource(); b.t->id = 44; return b; }
Resource* passthruResource(Resource* r) { return r; }

Resource* a9 = passthruResource(makeMoveBox().t);                 // A9 - no join at all
Resource* a5 = c > 0 ? passthruResource(makeMoveBox().t) : nul;   // A5 - through a '?:'
Resource* a6 = nul ?? passthruResource(makeMoveBox().t);          // A6 - through a '??'
```

Measured (`--run`, both binaries, all three cells). The garbage values are ONE sample - they are
reads of freed memory and differ per run; what reproduces is that none of them is 44 and that the
destructor has already run:

| binary | `a9->id` | `a5->id` | `a6->id` | dtors |
|--------|----------|----------|----------|-------|
| `a48829b` (pre-`joinlife`) | 630886976 | 630886976 | 630886976 | 1 each |
| `dd7b5b5` (`fix/joinlife`) | -823581590 | -823581590 | -823581590 | 1 each |

Expected 44. The destructor has already run when the local is read, on every cell and on both
binaries - the join spellings agree with the direct spelling, which is the bug.

## Root cause

`cflat/MainListener_Expressions.cpp` / the owning-temp escape guards
(`RejectOwningTempUniqueFieldEscape`, `RejectOwningTempUniqueFieldIntoSinkParam`) key off the
temp ledger identity of the value being bound. A borrowing callee returns a fresh `CallInst`,
which is not in `pendingOwnedStructTemps` and carries no `FromOwningTempField` marking, so the
declaration path never asks the question. `ParameterProvablyRetainsArgument` is the "does the
callee keep it" walk used at the CALL site; there is no matching "does the callee RETURN it"
walk feeding the declaration site.

## Fix direction

At the call site, when an argument IS a temp's unique field and the callee can return that
parameter (a `return <param>` walk, the dual of `ParameterProvablyRetainsArgument`), propagate
the `FromOwningTempField` marking onto the call result so the existing declaration guard fires
with its existing diagnostic. The accept set to freeze first: a borrowing callee whose result
does NOT derive from the parameter, and the read-only consumer `readResourceId(passthru(...))`,
which must stay legal (it is used inside a statement and the temp outlives it).
