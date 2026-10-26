# p3: call-result temp field pointer escaping through a call - remaining shapes after T510

**RULED 2026-10-25 (maintainer), item 1: REFUSE.** A pointer read from an owning call temporary's field passed to a callee that MAY store it into outliving memory (select / join with any outliving arm: global, heap, out-param) is a compile error, Rust-style ("borrow of a temporary may reach a longer-lived slot"); rewrite with a named owner (`H h = mk(); f(h.heap, c);`). The frozen Test/test_move.cb `temp_uniq_accept_join_dest_local_arm*` and `*_fresh_arm*` legs move to an err_ file; `*_both_local_arms*` (provably local) stays accepted and freed once. Keep-alive (leak) was rejected for this shape. Items 2-3 follow the same rule (unique param: refuse; ternary owner with Unknown callee: existing Unknown = keep alive stands). Buildable.

T510 landed the three-valued store walk: a raw pointer read from an owning call temporary's field and passed to a
CFlat call is refused when the callee provably stores it outside the call, destroyed at full-expression end when it
provably does not, and kept alive (leaked, never freed early) when the walk cannot tell (indirect callee, bound
exhausted, address escape, integer cast). Remaining shapes (repros in scratch/repro_keep/t510/, clang twins .cpp):

1. RULING NEEDED - unique-field select with a local arm (u_sel0.cb): a select joining a local and the global
   destination, run with c=0, stores the temp's unique-field pointer into the global and a later read is a
   heap-use-after-free on master, fix and clang. Both fixes are blocked by the frozen
   Test/test_move.cb `temp_uniq_accept_join_dest_*` legs: keeping the owner alive changes their freed-once count
   1 -> 0 (leak), refusing breaks that accepted set. Decide which behavior the frozen legs should pin.
   Site: cflat/LLVMBackend_OwnershipTemps.cpp ~885-891.
2. Raw call-temp field passed to a `unique int*` parameter double-frees (uptr_direct.cb, r8_uptr.cb; master too).
   The borrowed -> unique check refuses a named local but misses fields of call temporaries.
3. Ternary owner with an Unknown callee (tern_owner_unk.cb): same family, separate front-end shape (the owner is a
   ternary of two call temps); use-after-free on master, fix and clang.
4. Accepted leaks (Unknown verdict, by design): a local list<int*>.add(p) or a heap object freed in the callee
   (local_list / heap_local in the T510 review probes) keep the owner (d=0, clang d=1). Proving None cheaply would
   need container-aware summaries.
