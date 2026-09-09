# Relayed clang/LLVM diagnostic text is never localized

## Summary

Several cflat diagnostics quote text produced by clang's front end verbatim. The cflat
sentence around the quote goes through `LogError*` and is translated; the quoted clause
stays English in every locale, so a `zh-Hans` / `de` / `ja` user sees a half-translated
message. Today the only mitigation is the `clang:` prefix, which explains the English
island rather than removing it.

## Where it shows

`ReportUncompilableHeader` and `ReportOrphanHeader` in `cflat/LLVMBackend_CInterop.cpp`
(both build a `detail` string as `clang: <clang's own text> at <file>:<line>`), plus the
`clang-cl` compile/link failure relays in the same file and `LLVMBackend.cpp`, which
forward tool stderr as-is.

```
C++ header 'cpp_interop_broken.h' does not compile (clang: no type named 'string' in
namespace 'std' at .../cpp_interop_broken.h:14). Nothing in it can be bound until that
error is fixed.
```

Everything outside the parentheses is localizable; everything inside is not.

## Why it is not simply fixable

Clang has no message catalog. Its diagnostics are English format strings baked into
`clang/include/clang/Basic/Diagnostic*Kinds.td` (e.g. `err_typename_nested_not_found`,
`"no type named %0 in %1"`), expanded by `Diagnostic::FormatDiagnostic` with the
arguments already substituted. By the time cflat's `PrereqDiagConsumer` sees the text it
is one formatted English string with no id and no argument list, so there is nothing to
key a translation off. LLVM upstream has never carried i18n for this and is unlikely to.

## Proposed surface (needs a maintainer ruling before any build)

The user-visible question is what a non-English build should show. Options, roughly in
order of cost:

1. **Status quo plus the prefix (shipped).** Localized cflat sentence, English clause
   marked `clang:`. Zero cost, honest, permanently half-English.
2. **Localize the frame only, more aggressively.** Push more of the explanation into the
   translated half so the English clause reads as a quoted appendix - e.g. move the
   file:line out of the quote and into the localized sentence. Cheap; the diagnostic text
   itself stays English.
3. **Translate a curated subset.** Capture `DiagnosticsEngine`'s diagnostic ID (available
   on the `Diagnostic` object in `PrereqDiagConsumer::HandleDiagnostic`, before
   formatting) and add cflat `LogError*` strings for the handful of clang diagnostics that
   actually reach users through a header bind - missing type, missing include, unknown
   type name. Falls back to the English relay for anything unmapped. Real work, and the
   mapping ages with each LLVM bump.
4. **Do not relay clang text at all in non-English locales.** Replace the clause with a
   fully localized generic ("clang rejected a declaration in this header") and put the raw
   text behind `-v`. Fully localized, strictly less useful for debugging.

Recommendation to the maintainer: option 3 scoped to the header-bind diagnostics only, or
option 1 if the curated-subset maintenance is not worth it. Option 4 loses the actionable
detail and should not be chosen just to make the locale files look complete.

## Acceptance (once a surface is ruled)

- The chosen shape is applied to `ReportUncompilableHeader` and `ReportOrphanHeader`
  together - they must not diverge.
- Whatever clause remains English is still marked as relayed, so a reader knows why.
- `cflat/locales/` changes come only from the generator; no hand edits.
- `test.bat` Release, `test_lsp.bat`, `test_example.bat` stay green.
  `Test/errors/err_cpp_broken_header.cb` and `err_orphan_header.cb` cover both messages;
  keep their `expect_error` substrings on the localizable half.
