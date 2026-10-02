# Core libc prototypes use non-C types (strlen returns i32)

Found 2026-10-01 by V11 (user libc nobuiltin) while checking that real libc calls still fold.

## Summary
`cflat/core/cruntime.cb:496` declares `extern i32 strlen(const char* s);` - C says `size_t`
(u64 on LP64 and on Win64). Consequences:
- LLVM's TargetLibraryInfo rejects the prototype (signature check), so `strlen("abc")` is NOT
  folded to 3 at -O2 and other libcall optimizations on strlen are lost (perf, not correctness).
- A string longer than 2^31-1 bytes reports a truncated/negative length (correctness, rare).
Other core prototypes in the same section may have the same class of mismatch (e.g. `strncmp`
`n` as i64 instead of size_t/u64 - harmless on 64-bit targets, but check each against the C
signature).

## Fix direction
Audit the `extern` libc prototypes in cflat/core/cruntime.cb against the C signatures (size_t ->
u64, int -> i32, long -> target long). Changing strlen to u64 ripples to every core/user call site
that stores the result in an int - implicit narrowing at assignment rules apply; measure the blast
radius first (test.sh, examples, test_libs) and record it here before changing. Acceptance:
`strlen("abc")` folds to 3 in `--symbol-dump-opt function:main` at -O2.
