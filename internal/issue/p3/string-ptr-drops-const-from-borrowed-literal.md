# string borrows a const char buffer through a mutable `char* _ptr`

Summary: `string operator string(const char* s)` (core/string.cb) wraps `s` without copying and stores it as
`result._ptr = (char*)s`. `string.data()` then hands that buffer out as a mutable `char*`, so
`string s = "x"; s.data()[0] = 'y';` writes into read-only memory (string literals live in rodata; SIGBUS). Const phase 3
kept the cast and named the write: removing it needs the string pointee to be const.

Fix direction: make `_ptr` `(const char)*` and `data()` return `(const char)*` for borrowed strings (owned strings keep
a mutable path through an explicit method). Blast radius measured in const phase 3 r2: ~25 `char* x = s._ptr` sites in
string.cb plus every caller passing `data()` into a `char*` param. Natural home: const phase 6 (string literals become
`(const char)*`), see internal/plan/cflat-const.md.

Measured in const phase 6 (2026-10-18, part B attempt, reverted): with `_ptr` `(const char)*` and `data()` returning
`(const char)*`, 84 conversion sites appear (33 in string.cb, ~50 across ~20 other files); writes through `_ptr` / `data()`
stop the compile, so the real count is higher. Needs a design call first: `string_view` semantics and an owned-only mutable
accessor (owned vs borrowed flag). Site list: scratch/repro_keep/const_c06/partB_conversion_sites.txt (gitignored).
Literals are now `(const char)*`, so string.cb keeps three explicit casts (commented) until this lands.
