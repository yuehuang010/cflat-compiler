# `--symbol` with no source file parses BOTH platform backends and fails on a redefinition

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 11).
Reproduced on `cd847a3`, Release.

## Repro

```
cflat --symbol list
```
```
socket.posix.cb(56,14): redefinition of '_sockaddr_init'; this version is already defined at
socket.windows.cb(84). Two parameter lists that differ only by the name of a type (like 'int' and
'i32') count as the same overload, not two different ones.
```

No symbol output is produced. Passing any source file (`cflat app.cb --symbol list`) works. Note
the process still exits 0, so a script cannot tell this apart from a successful lookup.

## Root cause

`doc/CLI.md:380` documents the no-source-file mode: the compiler synthesizes an import-all-core
unit. That synthesized unit has no platform selection to anchor on, so the `.windows` and `.posix`
alternates of the same core module both get parsed and collide. `socket.cb` is the one that trips
first; `os.*.cb` is the same shape - the LSP bulk sweep already skips the non-host `os.posix.cb`
for exactly this reason.

## Fix direction

The synthesized import-all-core unit must apply the SAME host-platform alternate selection a real
compilation applies - pick `.windows` or `.posix` by host target, never both. One-place fix at the
synthesis site; the collision diagnostic itself is correct and should simply never be reachable
here. Exit non-zero when no symbol output is produced.

## Also from the same reporter, worth recording

`--symbol` was named the single most useful feature of the toolchain for this port - with no
standard-library API reference, it was how `list` / `dictionary` / `hashset` / `string` /
`stringbuilder` / `thread` were discovered. Two asks:

- Keep investing there. It is load-bearing for discovery, and the bare form is the first thing
  anyone types - which is why this bug is worth its low fix cost.
- A mode that lists a NAMESPACE's or a type's members without an exact name (prefix or wildcard
  search) would close the remaining discovery gap.

Consistent with the standing rule recorded in [[docs-gaps-from-external-backtester-report]]: LSP and
`--symbol`, not flat-file method lists, are the API discovery path.
