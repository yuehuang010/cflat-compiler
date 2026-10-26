# Chained compound assignment on returned lvalue is refused

Summary: Compound assignment results cannot be used as the left operand of another compound assignment.
Repro: `(mkK() -= 1) -= 2` for member && and free operator forms.
Master vs expected: master reports `not an addressable lvalue`; clang accepts.
Found: T455 r1 opus review, 2026-10-24 box, PRE-EXISTING
