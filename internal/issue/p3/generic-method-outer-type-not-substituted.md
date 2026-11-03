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

RULED 2026-10-31 (main session, from precedent; maintainer confirmed 2026-10-31): a tuple literal CONVERTS element-wise to the declared tuple
element types under the assignment rules - widening (`int` literal -> `i64` / `double`) accepted, narrowing refused like brace-init
(no-implicit-narrowing ruling 2026-09-04, return-follows-assignment 2026-09-04). Both parents agree (`std::tuple<long,int> t = {3, 4}`,
C# `(long, int) t = (3, 4)`). Test consequence: `(i64,int) t = (3, 4);` and `(double,int) t = (3, 4);` become accept legs in
Test/test_generics.cb; a narrowing literal tuple (`(int,int) t = (3.5, 4)`) gets an err_ leg.
