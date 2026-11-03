# Conditional skips copy of selected C++ lvalue

Summary: A conditional prvalue omits the copy when its selected arm is a C++ lvalue.
Repro: `c ? (Box)b : o` with `c` false.
Master vs expected: CFlat counters are `7/2/0/0/2/0`; clang counters are `7/3/1/0/3/0`.
Found: T454 r1 opus review, 2026-10-24 box, PRE-EXISTING

MIX10: not reproduced for the stated copy skip. Master matches clang (value and counts) for `Box r = c ? (Box)b : o;` (typed), `auto r = ...`, and `t454_take(c ? (T454Box)b : o)` (const&/&& overloads) with runtime `c`, both values, using the in-repo `cpp_t42_convref.hpp` T454Box counters.
Root cause (MIX10): a different symptom reproduces in a by-value sink. `t454_take_value(c ? (T454Box)b : o)` on master gives born/copies/moves/dead = 4/1/1/4 for both `c` values; clang gives 3/1/0/3. CFlat adds one ctor + move + dtor pair (copies the lvalue arm into a temp, then moves into the parameter slot); clang copies once directly into the slot. Not fixed: the TERNELIDE by-value path (`MainListener_Expressions.cpp` ~13862, auto-decl gated) is not the sink here. Needs a fresh repro and a separate issue.
