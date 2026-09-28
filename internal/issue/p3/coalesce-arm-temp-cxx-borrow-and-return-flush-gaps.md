# `??` / `?:` arm temporaries: C++ borrowing callee leaks, return flush frees an addressed arm

Follow-ups from the fix/coalesce-arm round-2 review (2026-09-27); probes scratch/revAK2/ (main checkout).
1. DropRetainedJoinArmPtrTemps treats every body-less callee as retaining, ignoring the C++ const-parameter
   release gate that already proves plain `peek(new T)` borrows: `cx.peek(new T ?? n)` and
   `cx.peek(c ? new T ?? n : n)` leak (as on master); `cx.peek(n ?? new T)` now leaks where master freed it.
   Fix: consult the same C++ borrow proof per argument before dropping arm temps.
2. The return flush frees a plain `?:` arm that the RETURNED pointer addresses:
   `int* f(int c) { return idp(c > 0 ? &(new T)->v : &y); }` - master leaked, now returns a dangling pointer.
   Same as master's `return idp(&(new T)->v)` (raw borrowed lifetimes are untracked by ruling), but the flush
   should keep a temp whose address flows into the return value, or the address-into-temp refusal should cover it.
3. Nested ternary in a return `return c ? one(c ? &(new T)->v ?? &y : &y) : one(&y);` leaks (master: use after
   free) - same class as the filed mixed-ternary leak.
