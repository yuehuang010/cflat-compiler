# Unique foreach element adoption leftovers (residue of p2 unique-foreach-element-and-param-copy-into-field-double-free)

Bucket: p3. Filed 2026-11-02 at the fe2field landing. The landed fix moves a `for (unique P* p in c)` element
(list<T>, array<T>, fixed array) out of its slot when a unique field adopts it (`t.p = p`, `?:` / `??` around p):
after the adoption the live slot (recomputed from the container's buffer/count, guarded by adopted == the
iteration's element) is nulled if it still holds the adopted pointer. Repros are inline below (scratch/repro_keep/fe2field/
is gone).

Fixed 2026-11-09 (fix/nforeach): item 1 (lambda capture). A by-value capture of a borrowed pointer (foreach
element or borrowed parameter) stays a borrow in the body. `t.p = p` inside the lambda is refused like the direct
store. `sink(p)` inside the lambda is refused for a borrowed parameter and for a `unique` foreach element (the
container owns it); it is accepted for a raw foreach element and for a capture whose outer binding was rebound to
an owned value in the same block before the lambda, as the direct form is. Tests: Test/errors/err_foreach_capture_adopt.cb,
err_foreach_capture_sink.cb, err_capture_param_adopt.cb; Test/test_move.cb testC15DataMutHeap (fe2_capture_* legs).

Fixed 2026-11-19 (fix/foreachadopt): items 1 and 4. (1) The lambda capture sink gate reads the collection's
element type (list<unique T*>, array<unique T*>, `unique T*[N]` own the element; their raw twins do not); the
loop declaration's `unique` decides only for a collection whose element ownership is unknown (views, interfaces,
C++ iterators). (4) A `?:` / `??` arm naming the active foreach element itself, in a call argument, records a
conditional move source (MainListener_Conditional.cpp CollectRangeElementArm), so an implicit `move` sink nulls
the slot on the taken arm before the call, as the direct `sink(p)` does; for a raw collection too. Consequence:
the arm also marks `p` moved, so a sink inside a nested loop is refused with "moved on an earlier loop
iteration", as the direct `sink(p)` already is. Tests: Test/test_move.cb testC15DataMutHeap
(fe2_capture_unique_decl_raw_list_*, fe2_tern_sink_*, fe2_coalesce_sink_*),
Test/errors/err_foreach_capture_sink.cb (raw loop declaration over list / array / fixed array, and through a
pointer). Round 2: a collection reached through a pointer (`for (R* p in a)`, a = list<unique R*>* /
array<unique R*>*) is classified from its type name too, and its loop variable binds the pointee's buffer slot
(RangeForContext headerStorage / headerType), so the direct sink, the `?:` / `??` sink and field adoption clear
the slot there as for a local collection (all were 134 on master). Legs fe2_ptr_list_* / fe2_ptr_array_*.
Round 3: a GLOBAL collection pointer ran zero iterations (master too): the range pointer is now loaded once at
loop entry, so count()/get() no longer receive the global's address. Legs fe2_global_ptr_*.

Open:

2. `a.removeAt(0)` (any shift) before the adoption: the adopted element now sits in another slot, the
   recorded index misses it -> double release. Measured 2026-11-09: segfault 139 on master and on fix/nforeach.
   Blocked: refusing removal/growth of the iterated container needs a mutator classification (CFlat has no const
   or mutator marker on library methods). Ruling needed before build.
3. Reading the loop variable `p` after the body grew the list (`p` is bound to the old slot address) reads freed
   storage (n_alias_grow, n_nested_inner). Measured 2026-11-09: segfault 139, intermittent (1 of 4 runs on
   fix/nforeach, 0 of 4 on master in the same sample). Same blocker as 2. The loop variable's storage is the list
   buffer GEP computed at iteration start (MainListener_Statements.cpp `rangeelemptr`), so it goes stale on growth.
5. Collections other than list<T> / array<T> / fixed arrays (C++ begin/end iterator protocol, other `_ptr`
   containers) get no slot clear (master behaviour). Not reproduced in native CFlat (no CFlat core container has
   begin()); needs a C++ header probe and a ruling on adopting out of a C++-owned container.
6. Pre-existing, out of scope of the 2026-11-19 fix: `sink2(c ? p : nullptr, p->id)` (move sink first, a later
   argument reads the element) is 134 on master, 139 after fix/foreachadopt. The shared conditional transfer
   path nulls the source slot before the later arguments are evaluated, so `p->id` reads null; the owning-local
   form `unique R* q = ...; sink2(c ? q : nullptr, q->id)` is 139 on master too. Plain `sink2(p, p->id)` runs
   (d=1 s=101). Repro:

   ```cflat
   void sink2(move R* q, int id) { if (q != nullptr) sunk = sunk + q->id * 100 + id; }
   list<unique R*> a = default; a.add(mk(1));
   for (unique R* p in a) sink2(yes ? p : nullptr, p->id);   // 139: p->id reads the nulled slot
   ```

RULED 2026-11-18 (maintainer): items 2 and 3 are LEFT AS IS, C++ behaviour. Mutating the iterated container inside
the loop body (removeAt / add / insert / clear / resize) invalidates the loop binding and is the user's bug, as
iterator invalidation in C++; no compile-time refusal, no mutator list or attribute, no version counter. Prior art
put to the maintainer: C/C++ undefined behaviour, C# runtime InvalidOperationException, Rust compile error.
Precedent: memory safety at function boundaries only (borrowed-pointer lifetimes inside a function are untracked).
Rejected: refusing a fixed core mutator list (the main session's pick). Consequence: items 2-3 are closed.
No doc/LANGUAGE.md paragraph (maintainer 2026-11-18: skip it); items 1, 4, 5 stay open.
(Items 1 and 4 fixed 2026-11-19, see above; item 5 stays open, needs a ruling.)

Repros (rebuilt 2026-11-19; R has a counting dtor, sink(move R* q) reads q, main returns the dtor count):

```cflat
// Item 3 (ruled, left as is): rc 139 on fix/foreachadopt 2026-11-19 (MallocScribble=1).
list<unique R*> c = default; c.add(new R());
for (unique R* p in c) { for (int i = 0; i < 64; i++) c.add(new R()); x = p->id; break; }
// Item 2 (ruled): the original removeAt repro is lost; `c.removeAt(0); t.p = p;` over two elements ran clean (2 dtors)
// in this session, so the 2026-11-09 shape differed. Item 5 (open): needs a C++ header with begin()/end().
```

Fix direction (superseded for 2-3 by the ruling above): for 2-3 the borrow rule says a container must not be mutated while a binding into it is live -
refuse growth/removal of the iterated container inside the body (Rust's borrow of the iterator), or re-find the
element by value.
