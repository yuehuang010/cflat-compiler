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

12. **V16 LANDED 19018a0c (2026-10-01).** Items 10-11 are fixed: whole-param and field-path aliases, alias-of-alias, `this` aliases, getter chains, free helpers taking the receiver pointer, `&elem` escapes at top level, postfix ++/--, hashset, and `_x` methods. Still missed after V16 r2; all master-equal, rc 133 or a corrupted caller. Probes are in scratch/repro_keep/v16/rv16b/ (selectors in each file).
    - Generic method calls `b.put<int>(1)`: isOwnMethod (~9779) does not see generic method bodies.
    - `->` is not split in splitLvaluePath/splitPath (o3 `l->add` via `&q->xs`, a14 `p->xs[0]=99`). Treat `->` as `.`.
    - Index stores through a whole-param alias (b1 `p.xs[0]=99`, c3): NameShadowedByEnclosingLocal (~10039) rejects alias roots. Skip it for wholeParamAliases roots.
    - Postfix ++ through an alias (a8, b2, o4): markContainerPath resolves only parameter roots.
    - A `this` alias passed to a helper (a5 `fillBox2(0, me)`).
    - Escaped element addresses beyond `int* q=&b.xs[0]; *q=v` (a9-a13, a15, a16). Robust rule: taking `&<param container path>[..]` counts as a write.
    - Index store into a direct `list<int>` by-value param, `l[0]=99` and `l[0]++` (separate from the struct-field cases).
    - FP nit: f6, an implicit `this` to a read-only free `peek(Box*)`, copies once.
13. **V17 LANDED 19018a0c (2026-10-01).** Item 12 is fixed: generic own methods, `->` paths, alias index stores, ++/--, field-path aliases (o4), `this` passed to helpers, element-address escapes, and direct-param index stores. Still open, master-equal; probes are in scratch/repro_keep/v17/rv17/:
    - x7: `list<int>*[1] arr; arr[0] = &b.xs; arr[0]->add(i)` gives rc 133. A container pointer stored into a local array is not tracked; element addresses are.
    - An unqualified `inner<T>(v)` inside a method gives "unknown generic function"; only `this->inner<T>(v)` builds. This is a separate resolution gap.
