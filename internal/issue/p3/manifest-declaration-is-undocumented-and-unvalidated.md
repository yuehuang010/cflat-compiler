# The `manifest` declaration is undocumented, and a bad `assemblyIdentity` fails only at process launch

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 10).
Confirmed on `cd847a3`: `grep -rn manifest doc/*.md` returns only `--isolated-manifest` and
`--vcpkg-manifest`; the file-scope `manifest ManifestDoc x = { ... };` declaration appears nowhere.

The feature itself was praised - it replaced a side-by-side `.manifest` file cleanly and gave
correct comctl32 v6 theming and PerMonitorV2 DPI awareness. Two problems around it:

## 1. Not documented

The reporter found it by reading `cflat/core/ui_native/win32.cb`, and only knew to look because
they had been told the feature existed. It needs a section in `doc/UI.md` (or `doc/LANGUAGE.md`,
wherever file-scope declarations are covered): the `ManifestDoc` shape, which fields map to which
manifest elements, and the two things people actually want it for - comctl32 v6 and DPI awareness -
as copy-pasteable snippets.

## 2. A bad `assemblyIdentity` produces a binary that will not start, with no compile-time signal

Including `assemblyIdentity` on the ROOT assembly with an empty `publicKeyToken` yields an exe that
fails at process launch with **Win32 error 14001** (`ERROR_SXS_CANT_GEN_ACTCTX`). The compiler
accepts it silently; the failure names no field, happens outside the compiler entirely, and there
is no way to inspect the generated XML. The reporter spent ~40 minutes bisecting.

Two fixes, either of which turns that bisect into a diagnostic:

- **`--dump-manifest`** - write the generated XML to stdout or a file and exit. Cheap, and useful
  well beyond this bug.
- **Validate the identity fields at compile time** - an `assemblyIdentity` with an empty or
  malformed `publicKeyToken`/`version`/`processorArchitecture` is an error at the declaration, with
  the field named. If the root-assembly identity is never useful, reject it outright and say so.

## Regression test

An `Test/errors/err_manifest_*.cb` for the rejected-identity leg once (2) is implemented; the
`--dump-manifest` output is a natural fixture for `test_lsp.bat`-style textual comparison, or for a
new leg in an existing UI test.
