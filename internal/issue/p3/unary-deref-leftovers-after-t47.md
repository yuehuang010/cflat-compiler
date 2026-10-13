# Unary `*` leftovers: implicit pointer conversion of a C++ class

Found by T47 review 4 (2026-10-09), pre-existing on master 608183756. Item 3 and the one-dimensional part of item 1
fixed by T95 (2026-10-10): `*arr` on a 1-D fixed array decays to its element pointer (native and C++ records), and
`*s` on a plain CFlat struct value is refused with "indirection requires a pointer operand".

Still open:
1b. `*arr` on a MULTI-dimensional fixed array (`int m[2][3]`, C++ records too): T95's decay is skipped when
   ConstInnerDimensions is non-empty (cflat/MainListener_Expressions.cpp ~16379), so a native array still fails
   LLVM verification and a C++ record array reaches the old refusal. clang decays the outer dimension to a pointer
   to the first row (row values 23 / 17 in the repros). Repros: scratch/repro_keep/t95/rev_t95_multidim.cb and
   rev_t95_cpp_multidim.cb (+ .cpp oracles). Fix: decay to a row pointer and keep the inner dimensions through
   the dereference and indexing; do not just drop the dimension check.
2. `*c` where the C++ class has an implicit conversion to a pointer (`operator C*()`): refused with "cannot
   dereference C++ value of type 'r4.Convert': its pointer conversion could not be lowered". clang converts and runs
   (0). Repro: scratch/repro_keep/t47/rev_t47_r4_convert.cb/.cpp. Root cause: CFlat has no lowering for a
   pointer-valued conversion operator at all. `ConvertViaImplicitConversionOperator` and
   `ApplyCxxConversionOperator` return null for a pointer destination (`CxxConversionOperatorTo` returns empty when
   `dest.Pointer`), and the member is not in the function table under "operator r4::C *" (a direct
   `CreateOverloadedFunctionCall` on the member name reports "function is not known"). Also refused: `C* p = c;` and
   passing `c` to a `C*` parameter. Fix direction: lower pointer-returning conversion members through the C++
   member call path (bind the member and call it on the operand's storage), then reuse for `*` and for `=` / args.
