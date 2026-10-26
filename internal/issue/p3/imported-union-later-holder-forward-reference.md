# Imported union cannot refer to a later holder type

Summary: A union in an imported file cannot name a holder declared later in the importing file; forward struct declarations also fail to parse.
Repro: Imported union names `XLater`, which is defined after the import; `struct Never;` forward declaration.
Master vs expected: master reports `cannot find the type XLater` and rejects the forward declaration; expected cross-file forward resolution and declaration support.
Found: T440 r2 opus review, 2026-10-24 box, PRE-EXISTING
