# Residual: parse errors still attach to the FOLLOWING token for some reserved words

The WORDING half and the wrong-hint half are FIXED (p3 bundle, off `819848e`), in
`cflat/CFlatErrorListener.h`:

- A keyword token where an identifier was expected now reports
  `'if' is a reserved word in CFlat and cannot be used as an identifier`
  (verified for `if`, `while`, `return`, `enum`).
- The blanket `hint: missing ';' at end of statement` is now emitted only when ANTLR's own
  message says a `;` was inserted, or when the line is genuinely unterminated. `p = default;`
  and other terminated lines no longer get a hint that points at the previous line; the real
  missing-semicolon case (`int x = 3` / newline / `int y = 4;`) keeps it.

## What is left

1. **Attribution.** When the parser consumes the reserved word as part of a type specifier, the
   error still lands on the FOLLOWING token and the reserved-word wording cannot fire:
   `int class = 3;` reports `found '=' but expected {'move', '(', Identifier}` at the `=`.
   Same shape for `true` (its token has no literal name in the vocabulary, so the reserved-word
   test does not recognise it). Fixing this needs the offending-token choice itself to change,
   not the message text.
2. **No regression test is possible for these.** A syntax error aborts the parse before the
   listener walk, so `expect_error` never arms - measured: the diagnostic prints, the
   expectation never fires, and the compile exits non-zero. Any coverage here has to be a
   golden-output test, which the suite does not have.

Note: the original symptom (`int package = 3;`) NO LONGER REPRODUCES - the import-clause soft
keyword work on master made `package` a plain identifier again. Use `if` / `while` / `enum`
for probes.
