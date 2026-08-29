# `json""` prefixed string literal (deferred feature)

Ruling 2026-08-28 (maintainer): the brace-escape ambiguity for JSON-ish literals will be
solved by a PREFIX LITERAL - `json"..."` - not by changing either existing string path.
Hold off; this is a later feature. This plan replaces
`internal/issue/p3/json-ish-brace-literal-still-typed-string.md` (deleted with this ruling).

## The problem this solves (from the closed issue)

- `char* j = "{\"k\":1}";` is falsely rejected ("cannot initialize pointer 'j' with a value
  of type 'char'...") because `ClassifyBrace` returns `Verbatim` for matched braces whose
  content starts with `"` or `\`, `HasInterpolation` still reports true for that kind, and
  the literal takes the format path and comes out a `string`.
- The two string paths disagree about `{{` / `}}` folding inside a `Verbatim` region, and the
  corpus depends on the disagreement: `Test/test_reflect.cb` `toJson_nested` ends in
  `...,\"zip\":12345}}` meaning two REAL closing braces. Routing it down the plain path folds
  them and the test fails (measured). So neither "fold everywhere" nor "never fold when not
  interpolated" is free - both change documented behaviour or break a pinned accept-set leg.

## Ruled direction

A `json"..."` literal: no interpolation, braces are literal, and the literal types like a
plain C string (so `char* j = json"{\"k\":1}";` works). Existing literals are untouched -
zero migration, and `test_reflect`'s pinned behaviour stays as-is.

## Open design points (settle before implementing)

- Relationship to the proposed `r"..."` raw prefix (`internal/issue/p4/raw-string-literal-prefix.md`):
  `r""` disables interpolation but keeps backslash escapes; decide whether `json""` is just
  `r""` plus a `char*`-compatible type, a distinct semantic (e.g. compile-time JSON validity
  check), or whether one prefix subsumes the other. One lexer mechanism should serve both.
- Type of the literal: `char*` vs `string` vs context-dependent.
- Escape rules inside `json""`: backslash escapes probably must stay active (`\"` is needed to
  spell JSON at all in a quoted literal).
- Lexer-only change; both `ParseDeclarationSpecifiers` copies untouched (same constraint the
  `r""` p4 entry records).

## Acceptance sketch

- `char* j = json"{\"k\":1}"; return j[0];` compiles and `j` prints `{"k":1}`.
- `json"{{"` contains two literal braces (no folding).
- `Test/test_reflect.cb` `toJson_nested` unchanged and green.
