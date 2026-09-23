Bucket: p2 (leak + stack free; measured 2026-09-23 on fix/owning-view-reassignment-release)

# A plain `T*` local: reassignment never releases the old block, and a conditionally owned pointer frees a stack address at scope exit

Sibling of the owning-view defect fixed on fix/owning-view-reassignment-release (which gave view
locals a runtime "owns its block" flag). Plain pointer locals have both halves of the same defect:

```cflat
S* p = new S();  p = new S();          // first block never destroyed or freed (HeapAudit leaks)
S x = default; S* q = &x; if (c) q = new S();   // c false: scope exit frees the address of x (abort)
```

Owner-ness of a `T*` local is a compile-time flag that follows source order, not the branch taken.
Fix direction: the same runtime owns-flag as views (see LLVMBackend_VariablesAndIR.cpp /
LLVMBackend_OwnershipTemps.cpp on that branch): set at declaration, updated on every ownership
change (assignment of `new`, `move`, a borrow), consulted by reassignment release, scope exit and
unwind. Acceptance: exact destructor counts and HeapAudit unchanged across the reassignment; the
conditional shape exits 0 for both branch outcomes; legs next to 3400-3419 in
Test/test_cpp_interop.cb.

Also measured on that branch, same family (not fixed): a new block assigned INTO a borrowed view
is never taken over (`S[] x = v; x = new S[2];`, a `T[]` / `move T[]` parameter assigned `new`,
`w ??= new` into a default view) and leaks; `S[] id(S[] x) { return x; }` loses the element
count (1 destructor instead of 3).
