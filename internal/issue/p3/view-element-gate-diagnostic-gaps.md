# Array-view element gate: three diagnostic gaps

Bucket: batch mode (wording and one message-routing gap; no new rejection). Filed 2026-09-04
from the q13/q15 reviews (d77dfdae, 5f693b7d).

1. With MORE than one overload candidate, a view argument whose element mismatches every
   candidate surfaces as the generic "no overload of 'f' matches" plus candidate list; the
   element-spelled message ("cannot pass an array view of 'u8' as parameter ... whose element is
   'int'") fires only for a lone candidate (deliberate: the scorer's lone-candidate gates in
   cflat/LLVMBackend_Overloads.cpp ~302 and ~330). Consider a post-resolution hint naming the
   element when every candidate failed on the element axis alone.
   ATTEMPTED 2026-09-05 and reverted from the wording batch: a post-resolution block that
   re-modelled the scorer's per-position accept (IsTypeMatch / IsTypePromotion /
   CompareUpconvert) emitted a FALSE hint when another position was refused by a gate it did
   not model (raw `int*` into a `T[]` parameter at position 1, element mismatch at position 0:
   "refused solely on that axis" while fixing the element still does not compile). Do not
   re-model the scorer. Record a per-candidate "sole disqualifier = view element axis" flag at
   the scorer's own gates (~302, ~338) and print it at the no-match site; true by construction.
3. Two of the three gate strings go through `LogErrorContext(ctx, std::format(...))` and never
   enter the localization catalog (same as the sibling raw-pointer-to-view gate); only the call
   door's `LogErrorMessage` is catalogued. Decide whether these belong in the catalog.

Deliberate, not a gap: `bool[]` <-> `u8[]` is rejected on i1-vs-i8 (zero users in the tree).
