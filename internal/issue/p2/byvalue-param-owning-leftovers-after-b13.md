# By-value owning param leftovers after B13

Summary: B13 (2026-09-29) made a by-value owning param that is returned or written whole a consume-inferred sink. For a copyable type it makes a copy instead. These diagnosed gaps remain:

1. **Closure param returned while the caller keeps its copy:** `Lambda f(Lambda f) { return f; }` gives rc 138 on master and on the branch (b13r2/cl3.cb).
   - Closures are excluded from the P1-1 return copy, because copying breaks err_bond_lambda_escape.
2. **Whole writes the collector misses (rc 133 on both):**
   - a write inside a lambda body, `Lambda<void()> l = () => { w = mkO(3); }` (b13r1/c3);
   - a write through a taken address, `S* p = &w; *p = t;` (b13r1/s6).
3. **C++ move-only param return spellings (after B22):** plain `return w;` moves like clang; `return (w);` and a `return w;` in an earlier branch are still refused where clang moves the parameter.
4. **From the B13 round-2 review (b13_review2.md in scratch/repro_keep/b13r2; probes in cflat-fix-b13 scratch/b13r2rev):** a mutating method on a copied by-value param's list field, `a.l.add(...)`, double-frees the caller's buffer after it grows.
5. **From the B22 round-2 review (scratch/repro_keep/b22/rev2/), pre-existing on master:**
   - `unique<T>` conditional moves are not tracked: `if (t) takeC(u); else takeC(v);` leaks the unmoved sibling and a later read of the moved one is not diagnosed; the ternary route `takeC(t ? u : v)` inherits it (e5, e6 leak one object; e10 / e14 rc 139).
   - Nested unique pass-through `takeC(passC(u))` and `takeU(passU(move u))` crash rc 133.
   - Binary operator on an alias-return (by-value param returned) non-copyable type used as a value, `BaT r = a ^ 1` and the RHS-return twin: refused with a pointer-to-value materialization error (review P3-2).

Related, filed separately: p2/alias-return-byvalue-param-leftovers-after-b21.md (B21).
