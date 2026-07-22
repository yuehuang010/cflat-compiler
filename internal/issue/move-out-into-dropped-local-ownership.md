# `T value = move slot; <drop>` mis-owns for borrows and unique interfaces

Opened 2026-07-21 during Stage 1 of `internal/plan/container-ownership-transparency.md`.
Blocks the `_releaseAt` half of the read-side collapse (dequeue collapsed fine; `_releaseAt`
could not).

## Summary

When a container releases an owned slot by moving it into a local and letting that local drop -
`void _releaseAt(int index) { T value = move _data[index]; }` - the DROP of `value` does the wrong
thing for two element kinds, in OPPOSITE directions:

| element T            | move-out-into-dropped-local does | correct behavior      | verdict |
|----------------------|----------------------------------|-----------------------|---------|
| `int`, value struct  | destructs local                  | destruct              | OK      |
| `string`, owning val | destructs local (frees buffer)   | destruct              | OK      |
| `unique T*` (thin)   | `delete` on drop                 | free                  | OK      |
| **bare `T*` borrow** | **`delete` on drop**             | **free NOTHING**      | **double-free** |
| **`unique IShape`**  | **frees nothing on drop**        | **free via vtable dtor** | **leak** |

So a bare-pointer borrow OVER-owns (spurious `delete`) and a `unique` INTERFACE (fat pointer)
UNDER-owns (no free). `unique T*` (thin pointer) and value/string are handled correctly.

## Repro

`queue<T*>` bare-pointer borrow, double-free at teardown (this is the exact crash that blocked
Stage 1 when `_releaseAt` was collapsed):

```cflat
import "queue.cb";
import "diagnostic/heap_audit.cb";
struct P { int v = 0; };
extern int main()
{
    HeapAudit.enable();
    queue<P*> q;                 // with _releaseAt collapsed to `T value = move _data[index];`
    q.enqueue(new P());
    q.enqueue(new P());
    P* a = q.dequeue();          // removes one; caller owns it
    P* b = q.peek();             // borrows the remaining slot
    delete a; delete b;          // caller frees both
    return 0;                    // ~queue -> _releaseAt(remaining) -> spurious `delete b` -> DOUBLE FREE
}
```

Backtrace (Release, libmalloc `POINTER_BEING_FREED_WAS_NOT_ALLOCATED`):
`~queue<P*>` -> `_releaseAt` -> `operator delete` on the already-freed `b`.

`queue<unique IShape>` leak (same collapse, remaining boxed element never freed): enqueue 2,
dequeue 1, the teardown `_releaseAt` on the remaining slot frees nothing -> HeapAudit reports a
leak of the boxed object.

## Root cause (hypothesis, needs codegen confirmation)

The `move` keyword confers ownership on the destination local UNCONDITIONALLY, without consulting
whether T is a borrow or an owning type:

- For a bare `T*` (a borrow the container never owned), `move` should degrade to a plain copy and
  the local must be a NON-owning borrow (drop = no-op). Instead the local is marked owning and its
  drop emits `delete`.
- For a `unique IShape` (a fat `{vtable*, data*}` owning value), `move` into a local should mark it
  owning so the drop runs the vtable dtor + free. Instead the drop is a no-op. Thin `unique T*`
  gets this right, so the gap is specific to the fat-pointer/interface representation.

The explicit forms the pre-spike `_releaseAt` used - `delete _data[index]` for `is_unique(T)` and a
no-op `{ }` for `is_pointer(T)` - are correct precisely because they do NOT route through the
move-into-local drop path. That is why `_releaseAt` still needs `if const` today.

## Fix direction

Make `move`-into-a-local respect the element type's ownership (the same "owns a resource" keying the
plan's gap 1 calls for on the param boundary):
- borrow types (bare `T*`, `alias T*`, bare interface value) -> `move` is a copy, local is a borrow,
  drop frees nothing;
- owning types (`unique T*`, `unique IShape`, value/string with a resource) -> local owns, drop
  frees exactly once, and for a fat-pointer unique the drop must dispatch the vtable dtor.

Once this holds, `_releaseAt` (and the analogous teardown paths in list/stack/dictionary/array)
collapse to `T value = move _data[index];` with zero `if const`. This is a READ-side sibling of the
plan's Stage 3 (gap 1) enabler and should be scheduled alongside it.

## Status

- `dequeue` collapse SHIPPED (Stage 1): sound because the moved-out value is RETURNED, never dropped
  in-body, so the mis-owning drop path is never hit.
- `_releaseAt` collapse DEFERRED: kept as `if const` until this gap is fixed. Enumerated in the plan.
