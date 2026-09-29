# By-value owning param leftovers after B13

Summary: B13 (2026-09-29) made a by-value owning param that is returned or written whole a consume-inferred sink. For a copyable type it makes a copy instead. Probes are in scratch/repro_keep/b13r1/ and scratch/repro_keep/b13r2/. These cells are still open:
1. **Closure param returned while the caller keeps its copy:** `Lambda f(Lambda f) { return f; }` gives rc 138 on master and on the branch (b13r2/cl3.cb).
   - Closures are excluded from the P1-1 return copy, because copying breaks err_bond_lambda_escape.
2. **Unary by-value operator that consumes its operand:** `O operator-(O x) { return x; }` then `-a` gives rc 133 on both. The unary path has no operand storage (b13r2/u1_unary.cb). The binary path was fixed in B13.
3. **Whole writes the collector misses (rc 133 on both):**
   - a write inside a lambda body, `Lambda<void()> l = () => { w = mkO(3); }` (b13r1/c3);
   - a write through a taken address, `S* p = &w; *p = t;` (b13r1/s6).
4. **C++ move-only param `return w`:** still refused with "cannot copy ... into the return slot" (b13r1/b7, x4). clang moves an implicitly-movable return.
5. **From the B13 round-2 review (b13_review2.md in scratch/repro_keep/b13r2; probes in cflat-fix-b13 scratch/b13r2rev). All crash on master too:**
   - Operator operands that are a field or a deref, `h.o + b` and `*pa + b`, still double-free. The call form `f(h.o, b)` runs clean.
   - A ternary of named owners passed to a consuming param double-frees, in both the operator and the call form.
   - A mutating method on a copied by-value param's list field, `a.l.add(...)`, double-frees the caller's buffer after it grows.
Related, filed separately: p2/alias-return-from-temporary-use-after-free.md (B21).
