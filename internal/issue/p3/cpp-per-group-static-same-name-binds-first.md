# C++ internal statics with the same name in two import groups: CFlat use binds the first group silently

Summary: V14 (2026-10-01) gives internal-linkage namespace variables one copy per import group. Inline bodies see their own group's values, which matches the clang two-TU oracle. A CFlat-side use of a name defined in two groups with different values (`constexpr int kv`, `static const int sv`, constexpr struct, `n.qk`, mutable `static int n.sm`) binds the FIRST import line's copy, with no diagnostic. Swapping the import order flips it. C++ in one TU would be a redefinition error.

Probe: scratch/repro_keep/v14/rv14c/q2.

Cause: CheckCxxNamespaceConflicts (LLVMBackend_CInterop.cpp ~15206) strips the __cflat_sv_ prefix, so two different declarations with the same mangled name compare equal. Folded scalars have no linkage name and never conflicted, not even before V14.

Fix direction: make the conflict identity the declaration's file:line (plus the mangled name), so a real ODR clash reports while the same header reached through two groups stays silent.

Also:
- With -v, every per-group static prints a false "C++ demand pass: '_ZL2ka' is still only declared", and stats.unresolved overcounts. The `want` check (CClangExtract.cpp ~6880) looks up the original name after the rename.
- Namespace-scope `constexpr int ka[3]` gives "Undefined variable" from CFlat, with no -v skip reason. This is likely an old limitation, but it is undiagnosed.

RULED 2026-10-01 (maintainer): a CFlat-side use of a same-name C++ internal-linkage static present in two import groups is REFUSED as ambiguous, naming both headers. No file:line identity scheme.
