# Free C++ operator via user conversion is not ranked against built-in candidates

Found: T219 Opus review 1 (2026-10-14). `A` has `operator int()`; `B(const A&)`; free `int operator+(const B&, int)`;
`a + 1`. clang++ -std=c++20: ambiguous (error). master: compiles, built-in route (rc 6). T219: compiles, free operator
via the B conversion (rc 7). Both accept code clang refuses; T219 changed the value master produced.
Repro + clang twin: scratch/repro_keep/t219_rev/c02_builtin_vs_free/.

Also pre-existing (same review): hidden friends are published as namespace-scope functions (CClangExtract.cpp
~2075-2082), so they are found without ADL - c06c (exact types) clang refuses, master/T219 return 52; c06a (needs a
conversion) clang refuses, master refuses, T219 returns 51. A global-namespace free operator over namespaced operands
is not found (c07d: clang 64, cflat "no operator").

Fix direction: when a free candidate needs a user conversion on a class operand whose class also converts implicitly
to a scalar that a built-in operator accepts, refuse as ambiguous (clang's built-in candidate set); publish hidden
friends as ADL-only (reachable only when their class is an operand).
