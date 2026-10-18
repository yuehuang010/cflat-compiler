# string borrows a const char buffer through a mutable `char* _ptr`

Summary: `string operator string(const char* s)` (core/string.cb) wraps `s` without copying and stores it as
`result._ptr = (char*)s`. `string.data()` then hands that buffer out as a mutable `char*`, so
`string s = "x"; s.data()[0] = 'y';` writes into read-only memory (string literals live in rodata; SIGBUS). Const phase 3
kept the cast and named the write: removing it needs the string pointee to be const.

Fix direction: make `_ptr` `(const char)*` and `data()` return `(const char)*` for borrowed strings (owned strings keep
a mutable path through an explicit method). Blast radius measured in const phase 3 r2: ~25 `char* x = s._ptr` sites in
string.cb plus every caller passing `data()` into a `char*` param. Natural home: const phase 6 (string literals become
`(const char)*`), see internal/plan/cflat-const.md.
