# Residual: no raw-string opt-out from interpolation

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 13).

## Done in the p3 bundle (off `819848e`)

**`-o out/x.exe` when `out/` does not exist.** The LEAF output directory is now created when its
parent exists (`LLVMBackend.cpp`, `checkOutputDir`, applied to `-o`, `--out-lli`, `--out-asm`,
`--isolated-manifest`). A deeper missing path is still a typo and still reports the original
message. Documented in `doc/CLI.md`.

## Still open

**No raw-string opt-out from interpolation.** Every string literal is interpolated, so emitting
JSON means doubling `{` / `}` in content that has no interpolation in it at all. Fix direction
unchanged: a lexer-level raw-string prefix (e.g. `r"{ \"a\": 1 }"`) that suppresses
interpolation for that literal, with no effect on any existing literal. Left out of the bundle
because the prefix spelling and its interaction with the escape rules is a language decision,
not a mechanical fix. Related: `json-ish-brace-literal-still-typed-string`.

## Rulings already recorded (do not re-file)

- Adjacent string literals are NOT concatenated - by design. If the raw-string form lands, a raw
  string plus `+` covers the long-format-string readability case.
- Incremental compilation was split out as `no-incremental-build-and-no-up-to-date-check`.
