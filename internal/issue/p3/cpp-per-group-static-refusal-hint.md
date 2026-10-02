# Per-group static refusal should tell the user to put the headers in one import group

## Ruling (maintainer, 2026-10-02 00:20, Q6)

A C++ internal-linkage variable reached through two import groups: an equal-valued constant is one entity (merged and folded, as landed in Q6). A mutable one (or a constant holding an address) is refused, and the fix is for the user to put the conflicting headers in the SAME import group, so the variable has one copy.

## Gap

Both refusal texts in NoteCxxGroupStatic (cflat/LLVMBackend_CInterop.cpp ~16110-16120) say "CFlat cannot tell which one is meant" but do not say how to resolve it. Append a hint naming the grouped spelling, e.g. `import them in one group: import cpp {"a.h", "b.h"};` with the two import headers filled in. Update the expect_error substrings in Test/errors/err_cpp_per_group_static_ambiguous.cb only if they pin the full text; locales regenerate from the LogError strings.
