# Plain class n::TT in one import and class template n::TT in another are accepted

Follow-up to fix/using-tpl (2026-09-27). CheckCxxNamespaceConflicts records classes and namespace-scope
class templates under the same qualified name, so a plain `namespace n { struct TT {}; }` in header A and
`namespace n { template<class T> struct TT {}; }` in header B read as the same entity; clang++ (one TU)
rejects the redefinition. Fix direction: tag template vs non-template in the conflict table entity key
(LLVMBackend_CInterop.cpp CheckCxxNamespaceConflicts); a forward-declared template + its definition must stay
silent. Cache bump if recorded data changes.
