# Named hidden-friend template call is rejected

Summary: A named call to a hidden-friend function template is not found through the C++ class argument.
Repro: `p.ff(a, 5)` for the T460 r1 hidden-friend template.
Master vs expected: master rejects; clang returns 36.
Found: T460 r1 opus review, 2026-10-24 box, PRE-EXISTING
