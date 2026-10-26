# `function<...>** pp = &f` on a named function is accepted

Summary: After T478 (`function<...>* p = &f` refused, ruling 2026-10-24 (a)), the double-pointer spelling still compiles.
Repro: `int inc(int x) { return x + 1; } int main() { function<int(int)>** pp = &inc; return 0; }`
Master vs expected: master and T478 compile it; clang rejects `int(**pp)(int) = &inc;`. Expected the same drop-the-& refusal
(or a type mismatch): `&f` is a `function<int(int)>` value, not a slot of a slot.
Found: T478 r3 Opus review, 2026-10-24 box 2, PRE-EXISTING.
