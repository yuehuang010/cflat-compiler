# Empty brace return passes check without a return value

Summary: An empty brace return lets a non-void function pass checking despite falling off its end.
Repro: `Q f() { return {}; }`
Master vs expected: master passes `--check`; expected missing-return diagnostic.
Found: T442 r2 opus review, 2026-10-24 box, PRE-EXISTING
