# C++ namespace conflict check leftovers (after C2)

Found 2026-09-29 by the C2 review; all accepted by cflat and rejected by clang (one TU), same on master.
- A class template and a function template with the same qualified name in two headers.
- A class or class template and a `typedef W<int> Name;` of the same qualified name in two headers.

Site: CheckCxxNamespaceConflicts (LLVMBackend_CInterop.cpp) identity tags: C2 tagged class vs
class-template; function templates and typedef-of-specialization names use other identities.
Fix direction: extend the identity comparison so those kinds conflict with a class/class template of the
same name; keep the C2 accept set (forward decls, redeclarations, specializations, repeated import) silent.
Probes: scratch/repro_keep/c2/c2rev/ (main checkout).
