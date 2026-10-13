# C++ operator set with ambiguous templates accepted when no candidate dropped

From T85 reviews 3 and 4 (2026-10-10), pre-existing on master. T85 relays clang's ranking refusal only
when a candidate dropped for an unrelated pointee. Without a drop, CFlat's own ranking still picks from
an operator set that clang rejects as ambiguous:
- free-only set of two operator templates equally ranked for `A + int*` (clang: ambiguous; CFlat 35);
- int* / void* / template<T>(T*) member set without a drop (rev_t85_r4_no_drop).

## Repro
scratch/repro_keep/t85/ (rev_t85_r3_* freeamb, rev_t85_r4_no_drop)

## Fix direction
When the CFlat operator set contains an operator TEMPLATE, let clang rank the whole set (the T85
clangDecides path in MainListener_Expressions.cpp ~13763 free / ~14472 member) instead of only after a
drop; keep the cheap path for template-free sets so no new clang request is added on the common path.
