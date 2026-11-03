# Tuple literal into a wider element type (design question)

Summary: a tuple literal does not convert element-wise into a wider declared tuple element type.
Repro: `(i64,int) t = (3, 4);` and `(double,int) t = (3, 4);` report "cannot cast an aggregate value"; a typed
i64 element variable passes.
Found: T433 r2 opus review, 2026-10-24 box, PRE-EXISTING

Root cause (MIX5): the outer-type substitution and bare-call forms did not reproduce as stated; the free
generic tuple return/param and bare-call tuple deduction failures were fixed in MIX8 (Test/test_generics.cb
testMix8TupleTemplates).

Root cause (MIX8): not fixed; tuple literal element conversion is a design question (needs a ruling before a
fix - whether a literal converts element-wise to a wider declared tuple element type).
