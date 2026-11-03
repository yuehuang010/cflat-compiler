# Shift operator leftovers after SHIFTCH

Found by the SHIFTCH Opus review (2026-10-30 box 7), master same. Repros scratch/repro_keep/shiftch/ (w.hpp for C++ cases).
- `(s << 2).v` (and `(s << 2 << 3).v`) on a C++ class whose `operator<<` returns a reference -> "Undefined variable v" (sd1.cb,
  sd2.cb; the single shift fails too, so this is member access on a parenthesized C++ reference result, not the chain). Not fixed:
  the `.v` postfix base is not resolved as a struct value. Fix direction: postfix member access on a parenthesized C++ reference result.
