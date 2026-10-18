# C++ virtual dtor with a deleted / private / ambiguous operator delete is accepted when its vtable is unused

Summary: T294 (2026-10-16) closed the USED-vtable case: a class whose vtable is used (constructed, new, by-value local)
now refuses with clang's own text ("clang: attempt to use a deleted function" / private member / multiple base
classes), cold and warm. Still open: a header whose class is never demanded (vtable unused) compiles; clang++
-std=c++20 rejects the header at the dtor definition.

Repro (T294 review 1, scratch/repro_keep/wt_t294/rev_t294_sol_tmp/unused_min.{cb,hpp}, round2.log for the
private-inherited and ambiguous variants):
```cpp
struct P { virtual ~P() {} static void operator delete(void*) = delete; };
```
```cflat
import cpp "unused_min.hpp";
extern int main() { return 17; }   // cflat: 17, clang: error
```

Root cause: LazyBodies::Skip skips the inline virtual dtor body, so Sema::CheckDestructor never runs for it;
LazyBodies::ResolveUsedVTableDeletes (cflat/CxxIncrementalGroup.cpp ~1309) walks only Sema::VTablesUsed.

Fix direction: validate operator delete lookup for skipped, defined, nondependent virtual dtors even without a used
vtable (lookup only, no eager body parse), relay the diagnostic the same way as the used case. Cost check: run
scratch/libs_perf.sh (torch/std headers have many virtual dtors). Leg: import-only expect_error at file scope in
Test/errors/err_cpp_new_private_operator.cb against a dedicated fixture header (shared headers leak the refusal).

## T333 attempt (2026-10-16, Luna xhigh r1, parked)

Patch + report + probes: scratch/repro_keep/box1017/t333/ (t333_r1_parked.patch).
- Approach tried: LazyBodies::Skip records skipped concrete virtual dtors whose hierarchy declares a class-specific operator
  delete; ResolveUsedVTableDeletes queues them beside the VTablesUsed set, and a deleted selected delete is diagnosed with
  DiagnoseUseOfOverloadedDecl. The private-inherited and ambiguous variants were then refused cold and warm. The direct
  `= delete` variant still compiled after three attempts.
- Test-structure conflict (needs a maintainer call before the next attempt): a fix that refuses at import time also
  refuses `Test/library/cpp_interop_vdtor_delete.h`. Test/errors/err_cpp_new_private_operator.cb imports that header at
  the top for its T294 use-site legs. Those legs then become unreachable: the first import errors before any expectation
  is armed. The existing legs would have to move to per-header import-time expectations, which edits an existing test.
