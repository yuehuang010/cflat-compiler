# By-value owning param leftovers after B13

Remaining after W7 Round 5 (2026-09-30). Landed in round 5: copy-on-entry is driven by a
scoped classifier (container mutator allowlist on list/dictionary/queue field paths, user
methods of the parameter's own struct recursed through `gts.structMethodBodies`, container
aliases, `&param...` escaping to a callee), `queue<T>.copy()`, and the tracked queue legs in
`Test/test_collection_leaks.cb`. Probes q1-q8, l2, lC, lD, w3-w6 pass; see
`scratch/briefs/w7_report.md` for the full table.

1. **S4 lambda whole-write leak:** `c3d.cb` (reference-captured owning struct overwritten in a lambda) leaks one object; measured `c=3/d=2` on both master and this branch.
2. **Literal-through-identity bond escape:** `esc4.cb` passes a bonded lambda literal directly to `id`; master exits 139 and this branch exits 0, but safety is not established and the path remains unaddressed.
3. **Conditional unique moved-source read:** a later read moved on a runtime path remains undiagnosed (`e14` exits 139); needs path-sensitive moved-state design.
4. **Unique ternary argument:** `u5t.cb` still double-frees (rc 133); out of scope for Round 2.
5. **Read-only unique parameter:** `u1.cb` still consumes the caller's value (rc 139); out of scope for Round 2.
6. **Unique parameter return:** `return u;` is still reported by review as an alias error; exact reviewer spelling needs a reproducible probe.
7. **Alias-return operator materialization:** binary operator on an alias-returned by-value non-copyable type used as a value is refused with a pointer-to-value materialization error.
8. **Classifier blind spots (master-equal, no copy emitted):** methods of a GENERIC user struct instance (bodies are materialized, not in `gts.structMethodBodies`); a container alias taken INSIDE a user method (`list<T>* p = &values; p->add(..)`); a field-path write through an alias (`T** pp = &b.node; *pp = ...`); a field-path alias handed to a callee (`list<int>* p = &b.xs; f(p);`, only whole-param aliases escape as writes); a mutator reached through a C++ or interface receiver. Each falls back to the pre-fix shared-buffer behavior.
9. **Alias passed to a callee is treated as a write:** `f(p)` with `p = &param` copies the param on entry even when `f` only reads; a read-only callee costs one copy, never a double free.
10. **More classifier blind spots (round-5 review, all master-equal; probes scratch/repro_keep/w7/rev8/):**
    whole-param pointer alias calls (`Box* p = &b; p->late();`, `p->xs.add(..)`: al.cb, al3.cb);
    `hashset` fields (not in the container kinds: hs.cb); index stores through operator[]
    (`b.xs[0] = 99`, `b.d[1] = 77`: w1.cb f9, w3.cb d1); a user method handing `&field` to a free
    function (`helper(&xs)`: w3p.cb). Each leaves the caller sharing storage (rc 133 on growth).
11. **Pointer escape and reference writes (narrow review 2026-09-30, master-equal; probes
    scratch/repro_keep/w7/rev9/ e1.cb, e2.cb):** a method returning `&xs` used as `lp()->add(i)`
    and a static `Box.sput(&xs, 1)` leave the caller with a reallocated buffer (typeAtFieldPath is
    empty for a non-field root, counted as no write) - worst of the set, use-after-realloc; element
    writes through references (`b.xs[0] = 99`, `b.d.get(1) = 99`, queue peek) reach the caller;
    `_`-prefixed container internals (`_grow`, `_rehash`, `_releaseAt`, ...) are callable from
    outside and not in the mutator allowlist (`b.xs._grow()` rc 139). Allowlist entry queue
    `free` is dead (no such method).
