# Move-sink does not consume owning-value RVALUE TEMPORARIES (struct temps, closures)

Opened 2026-07-21 during Stage 5 (list) of `internal/plan/container-ownership-transparency.md`.
This is THE blocker for the whole move-by-default container collapse workstream, and it made the
already-shipped queue/stack insert collapses LATENTLY UNSOUND (they were reverted; see below).

## Summary

The Stage-3 owning-value move-sink (a plain by-value `T value` param a body unconditionally moves,
which nulls the caller's source) correctly consumes:
- a NAMED owning local (`string`/owning struct/`unique`), and
- a `string` RVALUE TEMP (`s.copy()`, a concat) - handled by `UnregisterOwnedStringTemp`.

It does NOT correctly consume two other owning-value RVALUE-TEMP kinds:

| arg to a sink (`enqueue`/`push`/`add`) | expected            | actual                                  |
|----------------------------------------|---------------------|-----------------------------------------|
| named owning local                     | move, source nulled | works                                   |
| `string` temp (`s.copy()`, concat)     | move, buffer xfer   | works                                   |
| **owning value-STRUCT temp** (`x.copy()` where x is a struct owning a resource) | move, buffer xfer | temp's end-of-expr destructor STILL runs -> **double-free (rc 134)** |
| **fat-CLOSURE temp** (`(int x)=>x+a`)  | independent per slot| env NOT transferred/cloned -> all slots alias, then **crash (rc 138)** |

## Repro (against a queue whose insert is the collapsed `_data[slot] = move value` sink)

```cflat
struct Row { string name = default; Row copy(){ Row r; r.name = name.copy(); return move r; } };
// (a) owning value-struct rvalue temp:
queue<Row> q; Row lr = default; lr.name = "a"+"b"; q.enqueue(lr.copy());   // -> exit 134

// (b) fat-closure elements:
queue<Lambda<int(int)>> q; int a=100,b=200,c=300;
q.enqueue((int x)=>x+a); q.enqueue((int x)=>x+b); q.enqueue((int x)=>x+c);
q.dequeue()(1); ...   // all read 301, then exit 138
```

Both reproduce on the collapsed queue/stack. The OLD copy-on-insert (`_placeAt` -> `value.copy()`)
deep-cloned the struct/closure, so the old containers were SOUND for these kinds - the collapse
regressed them. `list` surfaces it loudly because the example/test suite exercises `list<Lambda>`
and `list<owning-struct>` (7 examples + 2 tests go rc-134 under the list collapse); queue/stack only
hid it because no suite exercises `queue<Lambda>`/`queue<struct-temp>`.

## Root cause

`ApplyMoveParamTransfer` (LLVMBackend.h ~10097): the sink-consumption path
- computes `argIsOwner` from a NAMED `CallerName` / owning flags, so an rvalue temp (no CallerName)
  fails `argIsOwner` -> `isOwningSink` is false -> the transfer never fires -> the temp is bitwise-
  copied into the slot AND freed at end-of-full-expression -> double-free;
- even when it does fire, it calls `UnregisterOwnedStringTemp` / `UnregisterOwnedClosureTemp` but NOT
  an owned-STRUCT-temp unregister (there is `RegisterOwnedStructTemp`/`FlushOwnedStructTemps` at
  ~1917/1923 but no unregister-on-consume);
- `ParamIsOwningSinkEligible` (MainListener.h) excludes `IsFunctionPointer`, so a `Lambda`/closure
  element is never even a sink candidate; a closure element needs move-transfer-of-env semantics
  (transfer the env to the slot, unregister the temp), which the collapse removed.

## Fix direction (the missing enabler - a new stage)

Extend the move-sink so consuming an owning-value RVALUE TEMP is sound:
1. `argIsOwner` must recognize an owned struct/closure rvalue temp (present in
   `pendingOwnedStructTemps` / `pendingOwnedClosureTemps` by its Primary/Alloca) as a consumable owner.
2. On sink consumption of such a temp, UNREGISTER it from the end-of-expression flush (add
   `UnregisterOwnedStructTemp`; ensure `UnregisterOwnedClosureTemp` fires) so it is not double-freed;
   the slot becomes the sole owner.
3. Make closure/fat elements sink-eligible (or special-case them) so `container<Lambda>` transfers
   the env into the slot instead of aliasing it.
This is the RVALUE-TEMPORARY analog of the Stage-3 param-boundary "owns a resource" enabler, and it
is a hard prerequisite for collapsing ANY container insert (queue/stack/list/dictionary) to
move-by-default. Add regression coverage for `queue/stack/list<Lambda>` and
`queue/stack/list<owning-struct>.insert(x.copy())` to the leak matrix as part of the fix - the
current matrix has a coverage hole here.

## Progress (2026-07-21)

STRUCT-TEMP half FIXED (case A). At the arg-lowering site (LLVMBackend.h ~14748) the
owning-struct rvalue temp was registered for end-of-expr destruction unless the param `IsMove`;
now it is also skipped for an inferred owning-value SINK param (`paramTakesOwnership`), so the sink
slot is the sole owner - no double-free. Verified: `rowSink(lr.copy())` (free-function sink) is
HeapAudit-clean; regression added to `Test/test_collection_leaks.cb` ("struct-temp sink ..."). This
makes the move-sink complete for owning-value STRUCT temps regardless of containers.

CLOSURE half FIXED as an owning-value flow (Increment 1, 2026-07-21). CORRECTION to the earlier
premise: `is_pointer(Lambda<...>)` is **FALSE**, not true. A closure takes the `value.copy()` arm of
container `_placeAt`, so containers ALREADY own closures (copy-on-insert deep-clones the env via
`__closure_fat_ptr.copy`; `__closure_fat_ptr.dtor` frees it on release). The container was never a
borrow for closures. A fat closure is an OWNING VALUE type exactly like `string`, and now flows
through the SAME owning-value machinery:
- `ParamIsOwningSinkEligible` (MainListener.h) ADMITS a fat closure param (`__closure_fat_ptr` /
  encoded `__fatfn`), rejecting only THIN C fn ptrs (`__c_fn_ptr` / encoded `__thinfn`, own nothing).
- `ApplyMoveParamTransfer` (LLVMBackend.h) treats a closure param as `paramOwnsResource` and a named
  closure local OR an owned closure rvalue temp (`IsOwnedClosureTemp`) as `argIsOwner`, so a sink
  consumes it (struct-zero + MarkVariableMoved for the local; `UnregisterOwnedClosureTemp` for temps).
- Use-after-move is now diagnosed at the closure INVOCATION site (the `[PFX-5]` indirect-call path in
  MainListener.h): `sink(move f); f(1)` is a compile error ("use of moved variable 'f'"), not rc 139.
- The dequeue-return closure-temp leak (bug d) is fixed: the owned-closure-temp registration gate
  (LLVMBackend.h ~14880) now keys on the concrete LLVM closure type + `!ReturnsAlias`, so a
  monomorphized generic-`T` closure return (`queue<Lambda>::dequeue()(...)`) is cleaned up, while an
  `alias T peek()` BORROW return is correctly NOT registered.

## Status

- STRUCT temps into a sink: FIXED (case A). CLOSURE elements as OWNING VALUES: FIXED (Increment 1) -
  sink (named local + rvalue temp), use-after-move diagnostic, and dequeue-return leak are all green
  under HeapAudit (`Test/test_collection_leaks.cb` closure legs + `Test/errors/err_closure_use_after_move.cb`).
- Increment 2 (CONTAINER LIBRARY COLLAPSE): queue.cb / stack.cb / list.cb / dictionary.cb INSERT
  DONE (2026-07-21). Per the maintainer's refined design, insert keeps ONE `if const (is_copyable(T))`
  split: a COPYABLE element (string, closure, copyable struct) COPIES (source intact), a NON-COPYABLE
  owning element MOVES by default. Closures land on the COPY arm (a closure is copyable), so
  `q.enqueue(f)` copies and leaves `f` usable - not consumed. dictionary needed no compiler change
  (its two consume paths funnel through an explicit `move V` sink). See
  `internal/plan/container-ownership-transparency.md` "OUTCOME" for the shape. Still open: the
  read-side `move-out-into-dropped-local-ownership.md` gap (`_releaseAt` stays `if const`).
- Sound compiler enablers kept: Stage 2 diagnostic, Stage 3 named-local move-sink, Stage 4
  generic-method sink wiring, the STRUCT-temp sink completion (case A), and now the closure
  owning-value flow (Increment 1).
