# `File` offsets are capped at 2 GB on every platform

Created: 2026-08-02 (split out of `ftell-fseek-long-width-on-windows` when that P1 landed)

## Summary

`core/filesystem.cb` narrows every file offset through `int`, so `File.size()`,
`File.tell()` and `File.seek()` silently return or accept wrong values for files
larger than 2 GB - on Windows, Linux and macOS alike:

```cflat
// filesystem.cb:34-37
win_int _fs_fseek(void* s, int offset, int origin) {
    return fseek(s, (long)offset, origin);
}
int _fs_ftell(void* s) { return (int)ftell(s); }
```

The public surface is `int` too (`File.tell()`, `File.size()`, `File.seek(int, int)`),
so widening the internals alone is not enough.

Note this is NOT the `long`-width defect that was fixed on 2026-08-02 - the externs now
faithfully bind C's `long`, which is 32-bit on Windows/LLP64 by definition. C's own
`ftell`/`fseek` cannot express a >2 GB offset on Windows at all; reaching one requires
binding different entry points.

## Repro

Create a file larger than 2 GB, open it, and read `f->size()`: the value comes back
truncated (or negative). Not covered by any test - the suite would need a sparse or
2 GB+ fixture.

## Fix direction

Bind the 64-bit spellings behind the existing `if const (__WINDOWS__)` seam:

- Windows: `_ftelli64` / `_fseeki64` (`__int64` offsets).
- POSIX: `ftello` / `fseeko` (`off_t`, 64-bit on both supported targets).

Then widen `_fs_ftell` / `_fs_fseek` and the `File` methods from `int` to `i64`. That is
a public API change - `File.tell()` / `File.size()` return type moves - so it needs a
sweep of callers in `core/`, `Test/` and `example/`.

## Related

`extern i32 strlen(const char* s);` (`cruntime.cb:491`) is the same "declared narrower
than C" family: `size_t` is 64-bit on both ABIs, so this truncates too. It is called out
here only so it is not lost - touching it ripples through all string code and is not part
of this issue.
