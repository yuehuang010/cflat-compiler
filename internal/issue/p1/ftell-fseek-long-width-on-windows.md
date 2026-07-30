# core `ftell`/`fseek` bind C `long` as pointer-sized, reading garbage on Windows

Created: 2026-07-14 (found while fixing atol-llp64-truncation-on-posix)

## Summary

`core/cruntime.cb` declares C's `ftell` and `fseek` with `win_size` (pointer-sized:
i64 on 64-bit) where C uses `long`:

```cflat
// cruntime.cb:39-40
extern win_int  fseek(void* stream, win_size offset, win_int origin);
extern win_size ftell(void* stream);
```

C's signatures are `long ftell(FILE*)` and `int fseek(FILE*, long offset, int origin)`.
Now that cflat's `long` is target-native (32-bit on Windows / LLP64, 64-bit on POSIX /
LP64), the correct binding is `long`, not `win_size`.

This is the same defect class as the `atol` bug, mirrored:

- **Windows (LLP64, `long` = 32-bit).** `ftell` is bound as returning i64, but the CRT's
  `ftell` only writes `eax`. The upper 32 bits of `rax` are whatever was left in the
  register - the caller reads a garbage-extended offset. `fseek`'s `offset` is passed as
  a 64-bit value to a callee that only reads the low 32 bits; harmless in practice, but
  the declaration is still wrong.
- **POSIX (LP64, `long` = 64-bit).** `win_size` and `long` are both 64-bit, so today's
  binding happens to be correct. No bug on macOS/Linux.

So the failure is **Windows-only**, and cannot be reproduced or verified from a macOS or
Linux host. That is why it was deliberately left unfixed when `atol`/`strtol`/`strtoul`
were corrected - see below.

## Repro (Windows only)

Not covered by any test today. On a Windows target, seek a file past its start and read
the offset back; the high half of the returned i64 is unspecified:

```cflat
import "filesystem.cb";
// f.seek(...) then f.tell() - the value round-trips through `int` in filesystem.cb,
// which masks the garbage today (see below), so a direct ftell() call shows it best.
```

Note the garbage is currently **masked** by the callers: `filesystem.cb` immediately
narrows through `int`:

```cflat
// filesystem.cb:36
int _fs_ftell(void* s) { return (int)ftell(s); }
```

The `(int)` truncation throws the garbage upper half away, so `file.size()` / `file.tell()
/ file.seek()` behave correctly by accident. The wrong declaration is still a live
landmine for any caller that uses `ftell` directly, or for any future 64-bit-offset work.

## Root cause

Same as `atol`: the extern was written against the pointer-size alias (`win_size`) rather
than C's `long`, back when cflat had no target-native `long`. Commit bf63ae2 added one
(`long` = i32 on Windows, i64 on LP64), so the faithful binding is now expressible.

## Fix direction

Re-declare against `long`:

```cflat
extern int  fseek(void* stream, long offset, int origin);
extern long ftell(void* stream);
```

The awkward part - and the reason this was not done alongside the `atol` fix - is that it
**changes the type callers see**: on a Windows target `ftell` starts returning a 32-bit
value where `filesystem.cb` currently expects i64. The `(int)` casts at `filesystem.cb:34`
and `:36` need re-checking against both widths, and the change cannot be exercised
end-to-end from a POSIX host. Do this on a Windows box, with `test.bat` green, rather than
blind.

While in there, consider whether 64-bit file offsets are wanted at all: on Windows the
64-bit spellings are `_ftelli64` / `_fseeki64`, and on POSIX they are `ftello` / `fseeko`
(with `off_t`). Binding those instead would make `file.size()` correct for files > 2 GB,
which the current `int`-narrowing path silently gets wrong on every platform. That is a
strictly larger change than fixing the `long` width, and is arguably the more valuable one.

## Related

- `extern i32 strlen(const char* s);` (cruntime.cb:491) also truncates - `size_t` is
  64-bit on **both** ABIs, so this is not a `long`-width bug, but it is the same "declared
  narrower than C" family. Touching it ripples through all string code, so it is called out
  here only so the next reader does not think it was missed.
