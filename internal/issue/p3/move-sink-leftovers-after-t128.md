# Move-sink leftovers after T128 (ternary / `??` / indirect sinks)

PRE-EXISTING on master (T128 reviews 1-2, 2026-10-11). The review probes went with the worktree;
items 1-3 landed in T227 and were removed; each remaining item is its own repro (P = struct with a destructor, mv takes `move P*`).

13. (T259 review, pre-existing; repros scratch/repro_keep/t259_rev/rev_t259_opus_cases/) a field of a BY-VALUE struct
    param consumed through `?:` / `??` (c38b/c38d), through a nested field `o.inner.p`, and through a named
    brace or tag initializer - FIXED T262b (RejectJoinedSourceIntoUniqueField). OPEN, same item, forms disagree
    with the plain store (both memory-safe, not refused): pointer param `S* s` with `t.p = f ? s->p : nullptr`
    compiles and nulls the caller's s.p (repro scratch/rev_t262b_opus_repro_ptrparam.cb; plain `t.p = s->p`
    is refused); `alias S a = GS; t.p = f ? a.p : nullptr` compiles and consumes GS.p (repro
    scratch/rev_t262b_opus_repro_aliasglobal.cb; plain `t.p = a.p` is refused). Also accepted on master:
    a stack-address arm `t.p = f ? &pv : nullptr` (c11) and a `unique P*` global stored into a unique field
    (`t.p = UG`, c21c/c21d). Refused on master and fix: `sink(f ? u : nullptr)` (c16) and `*(f ? u : q)` (c26).
    A two-field interface join `t.item = f ? s.item : s2.item` (c25d) now compiles and nulls only at run time,
    like the non-interface two-field join: a later read of the taken field derefs null instead of a diagnostic.

## Ruling 2026-10-15 (item 4): re-init clears the moved mark
`mv(s.p); s.p = new P(); s.p->v` is legal: reassigning a consumed field clears its moved mark, as for a local and as T230
already does for the `t.p = s.p` path. Direct and ternary sink forms alike. Check whether T230 already covers the call-sink
path before implementing (may be stale).
16. (coalfield review, pre-existing) a borrowing destination of `this.p ?? v` / an interface-typed field `?? v` still
    leaks the taken field (eager `??`-site clear kept for those two forms, T466 leg); consuming a field of a by-value
    struct param through `??` / `?:` aborts on the non-null path (see 13).
