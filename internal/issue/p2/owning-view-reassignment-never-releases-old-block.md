Bucket: p2 (leak; CFlat structs and C++ classes alike)

# Reassigning an owning array view never releases the block it held

Found 2026-09-23 (fix/cpp-class-operator-new) while pairing `new T[n]` / `delete` allocation
functions for C++ classes. Measured before and after that fix:

```cflat
T[] v = new T[3];
v = new T[2];      // the first block is never destroyed or freed (HeapAudit: leaked)
```

Applies to a CFlat struct element type as well as an imported C++ class, so it is the owning
view assignment path, not the C++ allocator pairing. Compare `unique T* p = new T(); p = new T();`
which releases the old value.

Fix direction: assignment into an owning view runs the element destructors and frees the old
block through the element type's paired delete (the helpers from fix/cpp-class-operator-new
for C++ classes) before storing the new one; null / moved-from views skip it. Acceptance: exact
destructor counts and HeapAudit.reportLeaks() unchanged across the reassignment, for a CFlat
struct and for a C++ class element type, in Test/test_cpp_interop.cb next to 3350-3366.
