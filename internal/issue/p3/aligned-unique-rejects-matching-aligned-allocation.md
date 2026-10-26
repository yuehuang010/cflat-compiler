# Aligned unique reports alignment mismatch for new allocation

Summary: A review found that aligned `unique` rejects allocation of its element type with an alignment-mismatch diagnostic.
Repro: `unique<uqp.W, 64>` with `new uqp.W(...)`.
Master vs expected: master reports alignment mismatch; expected behavior depends on the R5 ruling noted in the review, which was not present in the available issue/plan records.
Found: T473 Opus r1 review, 2026-10-24 box, PRE-EXISTING
