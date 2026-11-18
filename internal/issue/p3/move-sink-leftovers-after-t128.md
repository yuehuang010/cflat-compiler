# Move-sink leftovers after T128 (ternary / `??` / indirect sinks)

PRE-EXISTING on master (T128 reviews 1-2, 2026-10-11). The review probes went with the worktree;
items 1-3 landed in T227 and were removed; each remaining item is its own repro (P = struct with a destructor, mv takes `move P*`).

13. (T259 review, pre-existing; repros scratch/repro_keep/t259_rev/rev_t259_opus_cases/) a field of a BY-VALUE struct
    param consumed through `?:` / `??` (c38b/c38d), through a nested field `o.inner.p`, and through a named
    brace or tag initializer - FIXED T262b (RejectJoinedSourceIntoUniqueField). This round also fixed the borrowed
    pointer-parameter ternary mismatch: `t.p = f ? s->p : nullptr` now gets the same refusal as plain
    `t.p = s->p` (Test/errors/err_ternary_borrowed_pointer_unique_field.cb). The c25d two-field interface join
    now diagnoses a later read of the selected field (Test/errors/err_joined_unique_interface_field_read_after_move.cb).
    Refused on master and fix: `sink(f ? u : nullptr)` (c16) and `*(f ? u : q)` (c26).
    FIXED MSINK13 (2026-11-15): alias `?:` / `??` joins into a unique field, stack-address joins, and a plain
    `unique P*` global stored into a unique field (`t.p = UG`) now refuse with the plain store's diagnostic;
    legs in Test/errors/err_alias_store_field.cb, err_unique_stack_address.cb, err_unique_global_field_chain_decl.cb.

## Ruling 2026-10-15 (item 4): re-init clears the moved mark
`mv(s.p); s.p = new P(); s.p->v` is legal: reassigning a consumed field clears its moved mark, as for a local and as T230
already does for the `t.p = s.p` path. Direct and ternary sink forms alike. Check whether T230 already covers the call-sink
path before implementing (may be stale).
