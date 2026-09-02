# `new T[n]` reaches a `unique T*` FIELD through a `move T*` parameter and is freed as one object

Found 2026-09-02 by the round-2 Opus review of the unique<T> branch. Memory-safety class:
n-1 element destructors never run and a `new[]` block is released with scalar `delete`.

## Repro

```cflat
int gd = 0;
struct Y { int v = 0; ~Y() { gd = gd + 1; } };
struct Holder { unique Y* arr = default; };
void stash(Holder* h, move Y* p) { h.arr = p; }
int main()
{
    { Holder h = default; stash(&h, new Y[3]); }
    return gd == 3 ? 0 : 1;   // observed: gd == 1
}
```

Probe: scratch/rev2/movearrayfield2.cb on the branch. The direct store `h.arr = new Y[3]`,
the `return move r` into `unique<T>`, and passing the array to a `unique<T>` parameter are
all rejected (Test/errors/err_unique_array_view.cb); this indirection is the one open leg.

## Root cause

Raw-array count is per-LOCAL metadata (`AllocatedByRawNewArray`, `RawArrayLength`,
`RawArrayLengthStorage`; `HasRawNewArrayProvenance` in LLVMBackend_OwnershipTemps.cpp).
A `move T*` parameter carries no provenance, so `RejectRawHeapArrayIntoUniqueField` cannot fire
inside `stash`, and the call site cannot tell that `stash` stores its parameter into a unique
field: `IsOwningSink` / `IsConsumeInferredSink` exclude pointer parameters and
`ParameterProvablyRetainsArgument` proves escape, not a unique-field destination.

## Fix direction

Needs a maintainer ruling; three options, cheapest first:
1. Record a per-parameter fact "stored into a unique field" during the callee's body walk
   (same family as the move-inference facts) and reject at the call site when an argument with
   array provenance binds such a parameter.
2. Reject `new T[n]` (array provenance) bound to ANY `move T*` parameter. Sound but breaks
   legitimate array sinks (`move Y* mkArr` style callers, Test/test_core.cb runRawCount*).
3. Put the count with the value (allocation header written by `new T[n]`) so the field's
   destructor can free correctly. Largest change; also fixes container elements.

## Measured 2026-09-02 (spike before ruling; probes scratch/rt/p1-p9.cb on the branch)

The "no per-parameter fact" premise above is WRONG. A `move T*` parameter already carries a
hidden `i64 <name>.raw_array_count` ABI argument (`ParameterCarriesRawArrayCount`,
LLVMBackend_ControlFlowAndFunctions.cpp ~1313; producer `RawArrayCountArgument`; consumer
LLVMBackend_CodegenHelpers.cpp ~186-210 which allocas a `.raw_array_count` slot and sets
`AllocatedByRawNewArray` on EVERY move pointer parameter, -1 = scalar/unknown). `move T*`
returns carry the same count. Measured with `new Y[3]` (Y has a destructor, gd counts dtors):

| shape | gd | verdict |
|-------|----|---------|
| `void sink(move Y* p) { }` scope-exit cleanup | 3 | count honoured (runtime branch on the slot) |
| `sink(move Y* p) { inner(move p); }` forwarding | 3 | count forwarded |
| `sink(move Y* p) { delete p; }` | 1 | explicit `delete` ignores the slot (scalar dtor + free) |
| `sink(move Y* p) { delete[_] p; }` | 0 | documented no-dtor form, as designed |
| `stash(Holder* h, move Y* p) { h.arr = p; }` (this issue) | 1 | `reset(move T* p)` receives the count, `_p = p` drops it |

So `move T*` is NOT a downgrade to a bare `T*`: it is a counted owning pointer at the ABI level,
and the suite pins that (Test/test_core.cb `runRawCount*`, forwarding shapes). Only two consumers
drop the count: explicit `delete p` on a parameter (filed separately,
internal/issue/p2/delete-of-move-param-ignores-raw-array-count.md) and adoption into `unique<T>`
(this issue).

Option 2 measured: the blanket form breaks core immediately (`toString` hands `new i8[32]` to
`_strOwned(move i8* buf, ...)`, string.cb:80 - every test fails). Narrowed to destructible
element types with precise provenance (count slot present), it finds ZERO true positives in the
suite but 5 false positives, all because the count slot marks every move parameter
(test_move.cb:503 `freeTree(node->left)`, test_core.cb:2490 `sinkPtr(makePtrOwned())`,
test_list_ownership.cb:1336 `items.add(move p)`, btree.cb:1207 `_freeSubtree(n->children[i])`
hit by 3 error tests). A static rule cannot separate "move param that IS an array" from "move
param of unknown count" - that is exactly what the runtime slot is for.

## Recommendation (for maintainer ruling)

Option 4, not in the list above: a RUNTIME check at the adoption site. Every path into a
`unique<T>` value goes through a call to core `unique<T>(move T* p)` / `reset(move T* p)`, and
the overload call emission (LLVMBackend_Overloads.cpp ~1605) already computes the count
argument for that parameter. When the callee is a core unique<T> ctor/reset, emit
`if (count >= 0) trap("unique<T> cannot own a heap array")` beside the static gate that handles
the provable case. No ABI change, no core edit, no new per-parameter fact; it is the same
sanitizer class as the existing move-origin probes. Option 1 duplicates a fact the ABI already
carries; option 2 has no sound static form; option 3 rebuilds what `.raw_array_count` is.

Under the alternative model ("`move T*` downgrades to bare `T*`, zeroing the source") the count
ABI would be deleted, and then `new T[n]` of a destructible T bound to any `move T*` (parameter
OR return) must be rejected statically, which also retires the "return 'move X*' instead"
advice in err_unique_array_view.cb:23 and the `runRawCount*` legs. Trivially destructible
arrays (the string case) would still pass as bare pointers since `free` is count-agnostic. That
is a language capability removal, not a fix, and needs its own ruling.
