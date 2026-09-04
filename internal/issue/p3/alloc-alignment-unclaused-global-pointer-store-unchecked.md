# Over-aligned block stored into an unclaused GLOBAL pointer is freed with ordinary delete

## Summary

The allocation-alignment rule ("alignment travels in the type") is enforced at stores into
locals, parameters, struct fields, array elements and dereferenced slots (4223c66, e04729c, and
the member-8 commit closing internal/issue/p2/alloc-alignment-provenance-remaining-holes). A
GLOBAL pointer destination goes through none of those doors: `gslot = move alignedLocal;` with an
unclaused `E* gslot` compiles, and `delete[2] gslot` emits the ordinary `_operator delete` on the
64-aligned block (IR-confirmed 2026-09-03, probe scratch/q01m8_rev5_G1.cb). doc/LANGUAGE.md ~1628
notes the gap.

## Related open launder

`E** pp = &alignedLocal;` parks an over-aligned block behind an unclaused `E**` without passing
any store door, which is why `move *pp` sources are not treated as determinate provenance. An
address-of door comparing the source binding's clause against the destination pointee type would
close both this and the global case with one rule.

## Fix direction

Add the global-store destination to the same RejectFieldAllocAlignMismatch call in
ParseAssignmentExpression (cflat/MainListener_Expressions.cpp ~3116, the element / deref door):
`destination` is a GlobalVariable and the global's TypeAndValue.AllocAlignValue is unclaused.
Reuse the existing message. Leg in Test/errors/err_align_alloc_mismatch.cb; accept leg for a
declared `alignas(0, 64) E* gslot` global in test_core.cb runRawCount* family.
