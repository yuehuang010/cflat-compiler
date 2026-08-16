# Diagnostic localization plan

Status: PHASE 2 IN PROGRESS. Phase 1 is complete. The StateAndImports, Lookup, WinRT,
Interfaces, ControlFlow, EmitAndLink, CInterop, and Overloads diagnostic categories have been
migrated - 147 templates across 148 `LogErrorMessage` call sites. 115 legacy `LogError(` call
sites and the direct-print inventory remain, so the `= delete` migration guard is not yet armed.

Landed since the original draft:

- **The error-test run is no longer the inventory.** It reached only 55 of 147 templates, because
  a diagnostic is only collected if some test provokes it. `utilities/extract_diagnostics.py`
  statically scans every `LogErrorMessage` call site and is now the completeness source; the
  runtime pass is enrichment that supplies real `argumentExamples`. The extractor writes
  `en-pseudo.json` directly and carries observed examples forward, so the compiler pass must run
  first and the extractor last. It also reports unmigrated `LogError(` sites, non-literal
  templates, stale catalog entries, and keys with no example.
- **The extractor ports `NormalizeKey`/`CompactKey` to Python.** The C++ version stays
  authoritative: every run re-derives the key of each existing catalog entry and reports any
  disagreement, so drift between the two implementations is caught immediately.
- **Catalogs moved from `locales/` to `cflat/locales/`** and are deployed next to the compiler by
  CMake and by both release packaging scripts. `test.sh`, `test.bat`, and `test_err.bat` point at
  the new path.
- **All ten catalogs cover the full 147-key inventory**: `de`, `en-simple`, `es`, `fr`, `it`,
  `ja`, `ko`, `ru`, `zh-Hans`, `zh-Hant`, plus the generated `en-pseudo`.
- **The LSP honors the editor UI language.** `initialize.params.locale` (or
  `initializationOptions.locale`) is resolved by `DiagnosticLocalization::ResolveClientLocale` and
  applied to the whole backend pool; `CFLAT_LOCALE` still wins. The VS Code extension passes
  `vscode.env.language`.

Not done, and deliberately out of the current slice: regression tests for locale selection, a
`cflat.locale` editor setting that overrides the UI language, and the `zh-HK` resolver ordering
fix (the `zh` branch returns before the exact-tag probe, and the probe lowercases the tag, so a
`zh-HK.json` catalog would not be selected until both are addressed).

## Objective

Add localization for all user-visible compiler output while keeping canonical message templates
in the C++ source in English. Locale data is external JSON deployed next to the compiler.

The design deliberately does not introduce error codes, named error symbols, or IDs written
at diagnostic call sites.

## Current state

`LLVMBackend::LogError` is the central fatal diagnostic path. It currently receives an already
formatted English `std::string`, writes the diagnostic for CLI compilation, and routes it to
the LSP diagnostic sink when present. `LogErrorContext` supplies source locations before
calling that path. Parser diagnostics and several progress/debug messages have separate output
paths.

The source language is not the default display language. The default locale is the external
English catalog `en-simple`. The source template is only the canonical fallback and the text
shown by the migration-only `pseudo` pseudo-locale.

Every user-facing compiler print must eventually use a localized message API, including errors,
warnings, parser diagnostics, progress/summary output, verbose output, grammar/check results,
and compiler-generated notes. Linker, LLVM, Clang, operating-system, and crash-handler output
must be inventoried explicitly; if it cannot be localized, it must use an explicitly named raw
output path and be documented as an exemption.

## API shape

Keep the English template at the call site:

```cpp
LogErrorMessage("use of moved variable '{}'", { moved });
```

The new API receives the unformatted English template plus ordered string arguments. It then:

1. Derives the catalog key from the English template.
2. Looks up the translated template.
3. Formats the arguments into the selected template.
4. Sends the final text through the existing CLI or LSP diagnostic path.

During the transition, the existing `LogError(std::string)` body delegates to a private
`EmitError(std::string)` implementation so the new API can use the real reporting path without
depending on the legacy entry point. Once all call sites use `LogErrorMessage` or an explicitly
named raw path, delete the `LogError` declaration with `= delete` and remove its wrapper
definition. At that point a bad merge is a compile-time error.

After the initial migration slice is ready, replace the legacy declaration with the deleted
declaration below and use the resulting compiler errors to drive the remaining call-site
migration:

```cpp
void LogError(std::string message) const = delete;
```

This is a compile-time migration guard. Any unmigrated call site or bad merge that calls the
old API fails the compiler build immediately. Unlike a runtime assert, it cannot be optimized
out of Release builds and cannot be hidden by a code path that is not exercised. Do not keep a
runtime compatibility stub under the same name.

If a genuinely raw or externally generated diagnostic must remain English, introduce an
explicitly named separate path such as `LogRawError` and document each use. Do not make raw
fallback behavior implicit through `LogError`.

`expect_error` matching must happen against the canonical English template or rendered English
message before translation. Existing negative tests must remain valid when a non-English locale
is selected.

## Message inventory and migration coverage

The migration needs a complete inventory, not only a search for `LogError`.

Enumerate every user-visible output site, including:

- `LogError`, `LogErrorContext`, `LogWarning`, and parser error listeners.
- `std::cout`, `std::cerr`, `llvm::outs`, and `llvm::errs` in compiler and LSP code.
- CLI argument and validation errors, progress/summary messages, `--verbose` output, grammar
  output, `--check` output, cache/init output, linker-driver output, and crash diagnostics.
- Messages sent through `DiagnosticSink`, hint sinks, and LSP `Diagnostic` objects.

`utilities/extract_diagnostics.py` is the discovery inventory: it scans the C++ sources and
enumerates every `LogErrorMessage` template whether or not a test reaches it. The error-test
batch run with `--locale pseudo` is the enrichment pass - it contributes real argument values for
the templates it exercises, and its coverage gap (55 of 147 at the time of writing) is exactly
what the extractor's no-example report lists. After migration, the
deleted `LogError` API and the absence of an approved localized-output wrapper around direct prints
must make omissions visible during build/review. Every direct print must either be converted or
appear in an explicit raw-output exemption list.

The inventory must distinguish message templates from dynamic source text, user program output,
third-party tool output, and debug data. It must not translate text emitted by the compiled CFlat
program itself.

## Catalog key format

Catalog keys are deterministic lowercase keys. Short keys contain only ASCII alphanumeric
characters. Keys longer than 40 characters are compacted to the first 20 characters, `...`, the
last 20 characters, and a 16-hex-digit FNV-1a hash of the full normalized key.

Normalization of an English template:

1. Convert ASCII letters to lowercase.
2. Replace each `{}` placeholder with `arg0`, `arg1`, and so on.
3. Remove every remaining character that is not an ASCII letter or digit.
4. If the normalized key is longer than 40 characters, compact it as described above.

Example:

```text
use of moved variable '{}'
-> useofmovedvariablearg0
```

The normalizer must be shared by the compiler and catalog validator. Duplicate normalized keys
are an error because punctuation-only differences can otherwise collapse to one key. The
validator should report the conflicting English templates.

The key is only a derived catalog lookup key; it is not an error code or a source-level symbol.

## Hashing decision

Do not use a hash as the visible JSON key.

A hash would satisfy the no-space/no-special-character requirement, but it would be opaque to
translators, would make catalog review difficult, and would make a hash algorithm change a
catalog migration. It also introduces collision handling without solving the naming problem.

The implementation may hash the normalized key internally for a faster in-memory lookup, but
the JSON key remains the readable lowercase alphanumeric form. If normalized-key collisions
become a practical problem, the validator should reject them rather than silently choose one.

## JSON format

Locale files live under:

```text
cflat/locales/en-simple.json
cflat/locales/en-pseudo.json
cflat/locales/ja-JP.json
cflat/locales/zh-CN.json
```

Example:

```json
{
  "locale": "ja-JP",
  "messages": {
    "useofmovedvariablearg0": "移動済みの変数 '{0}'"
  }
}
```

The English source template remains the canonical source text, but it is not the default display
catalog. `en-simple.json` is the required default English localization and must contain every
inventoried key before the migration is complete. If an entry is missing at runtime, the
compiler may fall back to the source template, but the missing entry must be reportable by the
diagnostic coverage mode and by catalog validation.

Translation values use numbered placeholders (`{0}`, `{1}`, ...) so a language can reorder
arguments. The English call-site template continues to use the existing `{}` spelling. The
localizer validates that every translated template references only existing argument indexes.

## Locale selection and fallback

Use this precedence:

1. Explicit `--locale <locale>`.
2. `CFLAT_LOCALE`.
3. `en-simple`.

Do not silently select the operating-system locale in this first design. The default must be
deterministic and must always mean the checked-in `en-simple` catalog.

Add `--locale-dir <directory>` for development and installed-layout overrides. Otherwise search
for catalogs relative to the compiler executable, not the current working directory.

Add `--update-locale <locale>` as a collection mode for a source compilation or `--check` batch.
It records the localized templates encountered during that run, then writes or updates
`<locale>.json` under `--locale-dir`. It must preserve existing non-empty translated values and
add encountered keys with the source English template converted to numbered placeholders as
translation stubs. It must write valid deterministic JSON with stable key ordering.

The generated `en-pseudo.json` also contains `argumentExamples`, keyed by diagnostic key. Each
array is ordered by placeholder number (`{0}`, `{1}`, ...) and contains a representative value
observed during discovery, so translators can understand how each argument is used.

The default catalog remains `en-simple`. The pseudo-locale discovery output is written
to `en-pseudo`, for example:

```text
cflat --locale pseudo --update-locale en-pseudo --locale-dir cflat/locales --check Test/errors/err_*.cb
```

This command is the translator handoff step. Newly encountered entries are deliberately written
with the source template as the initial value, for example:

```json
"useofmovedvariablearg0": "use of moved variable '{0}'"
```

The compiler-side update is driven by the error-test compilation itself and needs no manifest or
source tree at runtime. It is complemented, not replaced, by `utilities/extract_diagnostics.py`,
which reads the C++ sources to supply the templates no test reaches. Run the compiler pass first
and the extractor last: the extractor preserves every `argumentExamples` entry already present,
while the compiler rewrite drops the extractor's `argumentNames` and `sites` fields.

### `pseudo` migration locale

`--locale pseudo` is a reserved test and migration pseudo-locale. It does not load a translated
catalog. For every localized message it prints the canonical source-template text, including
the normal argument substitution, so tests can prove which source message was emitted.

While `pseudo` is active, the localizer also checks the corresponding key in `en-simple.json` and
prints a clear warning stating whether the `en-simple` entry is present and non-empty. This
allows the test suite to exercise every diagnostic path while simultaneously reporting missing
default-English coverage. The warning is test/migration metadata and must not be treated as the
compiler diagnostic itself.

Missing locale files, missing individual translations, malformed JSON, and invalid placeholders
must all fall back to the English source template. Localization failures must never replace the
original compiler diagnostic with a second failure.

## Components and likely files

Add:

- `cflat/DiagnosticLocalization.h`
- `cflat/DiagnosticLocalization.cpp`
- `cflat/locales/en-simple.json`
- `utilities/extract_diagnostics.py` - static call-site inventory; writes `en-pseudo.json`.

Update:

- `cflat/LLVMBackend.h` - localization state and `LogErrorMessage` declarations.
- `cflat/LLVMBackend_OwnershipTemps.cpp` - central formatting, fallback, `expect_error`, and
  diagnostic-sink integration.
- `cflat/MainListener.h` and relevant listener implementation files - migrate context-aware
  diagnostics where structured arguments are available.
- `cflat/main.cpp` - register and apply `--locale`, `--locale-dir`, and `--update-locale`.
- `CMakeLists.txt` - compile the localizer and deploy the `cflat/locales/` directory beside the `cflat` executable.
- `cflat/LspServer.cpp` - resolve the editor UI language from `initialize` and apply it to the
  backend pool.
- `vscode-extension/src/extension.ts` - pass `vscode.env.language` in `initializationOptions`.
- `package_release.sh` / `package_release.ps1` - ship `locales/` alongside `core/`.
- `doc/CLI.md` - document locale options, the catalog workflow, and LSP locale selection.
- `doc/DIAGNOSTIC.md` - document authoring and translation rules.
- the test harness - run diagnostic coverage with `--locale pseudo` and fail or report according
  to the agreed missing-`en-simple` policy.

The existing `DiagnosticSink` signature can continue to receive final display text initially.
Adding a stable internal key to the sink can be considered later, but it is not required for
the first localization pass.

## Implementation phases

### Phase 1: localizer and one diagnostic

- Implement JSON loading, locale selection, key normalization, placeholder validation, and
  `en-simple` default loading.
- Add `LogErrorMessage` to the backend.
- Add runtime template collection driven by the error-test suite.
- Add `--locale pseudo` source-text output and missing-`en-simple` warnings.
- Add `--update-locale <locale>` and deterministic source-template stub generation.
- Keep the legacy `LogError(std::string)` stub during migration so it remains available while
  call sites are converted; after the migration inventory is clear, change it to `= delete` and
  use compiler failures to catch any remaining call sites or bad merges.
- Migrate one representative diagnostic with one dynamic argument.
- Verify CLI and LSP output.

### Phase 2: migrate compiler diagnostics

- Inventory direct `LogError(std::format(...))` and `LogErrorContext` call sites.
- Inventory every direct user-facing print and every diagnostic sink, not just error calls.
- The first migrated categories are `LLVMBackend_StateAndImports.cpp` and
  `LLVMBackend_Lookup.cpp`; their templates are exercised by the error-test pass.
- The current migrated tranche also covers `LLVMBackend_WinRT.cpp`, `LLVMBackend_Interfaces.cpp`,
  `LLVMBackend_ControlFlowAndFunctions.cpp`, `LLVMBackend_EmitAndLink.cpp`,
  `LLVMBackend_CInterop.cpp`, and `LLVMBackend_Overloads.cpp`.
- Convert them category by category to structured templates and ordered arguments.
- Treat every deleted-function compiler error for `LogError` as an incomplete migration or bad
  merge; do not reintroduce a runtime stub to restore the old unlocalized behavior.
- Preserve source locations, `LogError` throw/exit behavior, and `expect_error` semantics.
- Route genuinely preformatted or third-party messages through an explicitly named raw-error
  path, if they are still in scope.

### Phase 3: catalogs and deployment

- DONE: keep `en-simple` updated from the extractor plus the error-test pass, with English source
  templates as initial translation values; refine wording where needed.
- DONE: nine non-English catalogs (`de`, `es`, `fr`, `it`, `ja`, `ko`, `ru`, `zh-Hans`,
  `zh-Hant`), verified end-to-end through the LSP.
- DONE: deploy catalogs in Debug, Release, packaging (`package_release.sh` / `.ps1`), and
  worktree test layouts.
- Add catalog validation to the developer workflow.

## Verification

Extend existing test files rather than creating new compiler integration test files. Verify:

- Default diagnostics remain English.
- Default output comes from `en-simple`, not directly from source literals.
- A selected locale translates a migrated diagnostic.
- `--locale pseudo` prints canonical source text and reports whether each `en-simple` key exists
  and is non-empty.
- The extractor enumerates every `LogErrorMessage` call site, and the error-test run records real
  argument values for the subset it exercises.
- `--update-locale <locale>` preserves existing non-empty values and adds encountered keys with
  the source English template and numbered placeholders as translation stubs.
- Catalog update output is deterministic and follows `--locale-dir`.
- Every direct user-facing print is either localized or listed in the raw-output exemption list.
- Missing catalogs and missing entries fall back to English.
- Malformed JSON does not hide the compiler diagnostic.
- Placeholder order can differ in a translation.
- Invalid placeholder indexes are rejected by validation and ignored at runtime.
- Normalized-key collisions are reported, and compact-key hashes preserve distinct long keys.
- `expect_error` passes under both English and non-English locales.
- `--check` batch mode still reports the correct files and failure count.
- LSP diagnostics receive translated display text without duplicate console output.
- Backend reuse and concurrent LSP analysis do not share mutable locale state incorrectly.

Run the host test suite after implementation, including the warm-cache path because the backend
is reused across `--check` and LSP analyses.

## Risks and mitigations

- **Normalized-key collisions:** reject collisions during catalog validation and include both
  source templates in the diagnostic.
- **Translation changes placeholder order:** use numbered placeholders in locale values and
  validate indexes.
- **Existing tests depend on English wording:** match `expect_error` before localization and
  keep the source template as the fallback even though `en-simple` is the default catalog.
- **Messages are missed by the migration:** run the complete error-test suite with `--locale
  pseudo`, require every exercised diagnostic to be collected, and require direct-print
  exemptions to be explicit.
- **Default English coverage drifts:** run the error-test pass with `--locale pseudo`, warn for
  every missing `en-simple` entry, regenerate with `--update-locale`, then run the extractor to
  re-add the templates no test reaches.
- **Locale file is unavailable in an installed build:** locate catalogs beside the executable
  and fall back silently to English with optional verbose reporting.
- **Unmigrated call sites bypass localization:** delete `LogError(std::string)` and require every
  compiler failure to be fixed or explicitly moved to `LogRawError`.
