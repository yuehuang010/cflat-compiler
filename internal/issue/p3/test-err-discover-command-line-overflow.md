# `test_err.bat --discover` silently regenerates nothing: the file list overflows cmd's line limit

Bucket: batch mode (one site in a batch script; no compiler change). Filed 2026-09-05 by the
review of fix/extern-conflict.

## Summary

`test_err.bat --discover` is the canonical step that regenerates `cflat/locales/en-pseudo.json`
from the `LogError*` format strings (CLAUDE.md, Localization). It accumulates every
`Test/errors/err_*.cb` into one `DISCOVERY_FILES` variable (`test_err.bat:193-194`) and passes
the whole list on a single command line (`:195`). With 361 error files the variable is 8174
chars, at cmd's 8191 cap: files sorting after `err_manifest_local.cb` are dropped, and the
invocation itself fails with `The command line is too long.` The `if errorlevel 1` on the next
line does not see that failure, so `--discover` exits 0 having written nothing.

Measured on master 9c9b0f9 with an instrumented copy of the script: zero `PASS` lines from the
main invocation, `en-pseudo.json` unchanged. Running the same generator in chunks
(`ls Test/errors/err_*.cb | xargs -n 25 cflat.exe --locale pseudo --update-locale en-pseudo
--locale-dir cflat/locales --check -i Test/library --nologo`) regenerates the catalog and is
idempotent - that is how the extern-conflict change's three new keys were produced.

## Fix direction

Batch `DISCOVERY_FILES` (e.g. 25 files per invocation) inside `:Discover`, and fail the step on
a non-zero exit from any batch. Acceptance: `test_err.bat --discover` on a tree with a new
`LogError` format string adds its key to `en-pseudo.json`; a second run changes nothing.

Related: `cflat/locales/en.json:134` still carries the retired key for the old
"conflicting declaration of extern" wording; the translation workflow owns that cleanup.
