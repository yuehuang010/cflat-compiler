# Unique foreach element adoption leftovers (residue of p2 unique-foreach-element-and-param-copy-into-field-double-free)

Bucket: p3. Filed 2026-11-02 at the fe2field landing. The landed fix moves a `for (unique P* p in c)` element
(list<T>, array<T>, fixed array) out of its slot when a unique field adopts it (`t.p = p`, `?:` / `??` around p):
after the adoption the live slot (recomputed from the container's buffer/count, guarded by adopted == the
iteration's element) is nulled if it still holds the adopted pointer. Repros: fe2field worktree reviews, copied to
scratch/repro_keep/fe2field/ (main checkout, untracked).

Fixed 2026-11-09 (fix/nforeach): item 1 (lambda capture). A by-value capture of a borrowed pointer (foreach
element or borrowed parameter) stays a borrow in the body. `t.p = p` inside the lambda is refused like the direct
store. `sink(p)` inside the lambda is refused for a borrowed parameter and for a `unique` foreach element (the
container owns it); it is accepted for a raw foreach element and for a capture whose outer binding was rebound to
an owned value in the same block before the lambda, as the direct form is. Tests: Test/errors/err_foreach_capture_adopt.cb,
err_foreach_capture_sink.cb, err_capture_param_adopt.cb; Test/test_move.cb testC15DataMutHeap (fe2_capture_* legs).

Open:

1. Residue of the lambda capture fix. (a) `for (R* p in c)` over a `list<unique R*>` with a raw loop declaration:
   the captured sink still traps (134, master too). The gate reads only the loop declaration's `unique`; the
   collection's element uniqueness is needed. (b) `for (unique R* p in c)` over a raw `list<R*>`: the captured sink
   is now refused, master ran rc 0 and the direct form still does. Same missing collection check, other direction.
2. `a.removeAt(0)` (any shift) before the adoption: the adopted element now sits in another slot, the
   recorded index misses it -> double release. Measured 2026-11-09: segfault 139 on master and on fix/nforeach.
   Blocked: refusing removal/growth of the iterated container needs a mutator classification (CFlat has no const
   or mutator marker on library methods). Ruling needed before build.
3. Reading the loop variable `p` after the body grew the list (`p` is bound to the old slot address) reads freed
   storage (n_alias_grow, n_nested_inner). Measured 2026-11-09: segfault 139, intermittent (1 of 4 runs on
   fix/nforeach, 0 of 4 on master in the same sample). Same blocker as 2. The loop variable's storage is the list
   buffer GEP computed at iteration start (MainListener_Statements.cpp `rangeelemptr`), so it goes stale on growth.
4. Implicit `sink(yes ? p : nullptr)` into a `move P*` parameter double releases. Measured: trap 134 under
   MallocScribble=1 (133 without) on master and fix/nforeach (`sink(yes ? p : p)` and explicit `sink(yes ? move p : nullptr)` are fine; plain `sink(p)` nulls
   the slot before the call, IR `store ptr null, ptr %rangeelemptr`). Root cause: the ternary arm collector
   (MainListener_Conditional.cpp ParseTernaryBranchesBody, collectConditionalMoveSources) records no move source
   for a foreach element arm. Two blockers found in the attempt (reverted): (a) a `nullptr` arm never reaches that
   collector (`yes ? p : nullptr` and `p ?? nullptr` take another path), (b) the foreach element NamedVariable does
   not carry IsUnique for `unique Res* p`; RangeForContext.elementDeclaredUnique now carries the declaration's
   `unique` for the capture gate, so a unique gate for this path can read it too.
5. Collections other than list<T> / array<T> / fixed arrays (C++ begin/end iterator protocol, other `_ptr`
   containers) get no slot clear (master behaviour). Not reproduced in native CFlat (no CFlat core container has
   begin()); needs a C++ header probe and a ruling on adopting out of a C++-owned container.

Fix direction: for 2-3 the borrow rule says a container must not be mutated while a binding into it is live -
refuse growth/removal of the iterated container inside the body (Rust's borrow of the iterator), or re-find the
element by value; 1 and 4 route through the same adoption hook.
