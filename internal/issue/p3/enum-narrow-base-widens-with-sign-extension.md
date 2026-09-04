# enum with a narrow unsigned base widens with sign extension

## Summary

`enum Small : u8 { B = 200 }; Small s = Small.B; return (int)s;` yields -56 on master; clang gives
200. The enum's declared base is `u8`, but the widening to `int` sign-extends, so any enumerator
above 127 reads negative once widened. Found by the round-2 review of c56efdf (probe
scratch/q03m1_rev2_p3.cb); identical before and after that commit, so unrelated to promotion.

## Fix direction

Find where an enum value's `TypeAndValue` reports signedness (IsUnsignedInteger on the enum type
should defer to the base type) and where the widening cast picks sext/zext for enum sources
(Upconvert / CreateCast in LLVMBackend_VariablesAndIR.cpp). Add legs in the existing enum test for
`: u8` with 200, `: u16` with 40000, `: i8` with -1 widened to int, and comparison `s > 100`.
