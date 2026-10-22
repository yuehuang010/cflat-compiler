# p2: view ownership decided at run time escapes the T377 sink gate (non-local storage, if-merge without a join)

Pre-existing; found by the T377 review (2026-10-21). Repros: scratch/repro_keep/box1021/t377_rev/.

1. A mixed owned/borrowed `?:` join stored into a struct field, a global, or an implicit-this field, then moved into a `move S[]` sink,
   double-frees when the borrowed arm was taken (D_field_join, P_same_name_global, Q_method_field; rc 134 under ASan on master). The same
   stores without the sink are clean. The T377 guard (LLVMBackend_WinRT.cpp ~1716) returns early for non-local storage, and non-local
   storage carries no runtime own flag.
2. `S[] v = w; if (c) v = new S[2]; sink(move v);` double-frees when the branch is skipped (N_cond_owned_if). OwnFlagMayBeFalse is set
   only by a `?:` join, never by an if-merge where the arms disagree on ownership.

Fix direction: (1) refuse the sink move from non-local view storage whose last store may be borrowed (conservative, same text as the
borrowed-view sink refusal), or give such storage a runtime flag; (2) set the may-be-borrowed bit at an if/switch merge where the arms
disagree, symmetric to the T377 r2 clear-on-all-arms rule.

3. No `?:` at all: `v = new S[1]; v = w; sink(move v);` after the view held an owned block double-frees (T377 review r2:
   scratch/repro_keep/box1021/t377_rev/c1_nojoin_reown_then_borrow.cb rc 134 at argc 6; c2_join_straight_reown_borrow.cb rc 134 at every
   argc). A straight-line re-assign from a borrowed view must leave the view borrowed (sink move refused like `S[] v = w; sink(move v);`).
