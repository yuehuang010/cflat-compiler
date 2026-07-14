# core `atol` truncates to 32 bits on POSIX (LP64)

`core/cruntime.cb` declares C's `atol` with a 32-bit return, hardcoding the Windows
LLP64 rule that `long` is 32-bit:

```cflat
// cruntime.cb:543-546
// ... Windows is LLP64, so C `long` is 32-bit: atol maps to int and atoll/strtoll to i64.
extern int    atol(const char* str);
```

On macOS and Linux (LP64) libc's `atol` returns a 64-bit `long`. Binding it as `int`
means the caller reads a truncated result.

## Repro

Not covered by any test today (`test_core` / `test_crt` exercise `atoi`/`atoll`, never
`atol`). A direct call would show it:

```cflat
import "cruntime.cb";
// On POSIX, atol("4294967296") should be 4294967296; the int binding truncates it to 0.
```

## Root cause

The declaration is platform-blind. The *compiler* side of this same LLP64 assumption
was fixed in `MapCTypeToTypeAndValue` (`LLVMBackend.h`), which now maps C `long` /
`unsigned long` to i64/u64 when `!targetWindows_`, so C headers bound via `import` get
the right width. The hand-written core extern was not updated with it.

Note the same comment block covers `strtol`, which should be audited alongside `atol`.

## Fix direction

The awkward part is that the return TYPE differs per platform, so a plain
`if const (__WINDOWS__)` around two `extern` declarations changes the type callers see.
Options:

1. Declare it per-platform behind `if const (__WINDOWS__)` (`int` on Windows, `i64`
   elsewhere) and let callers use the platform-correct width. Widest-compatible callers
   should assign to `i64`.
2. Drop the `atol` binding entirely and point users at `atoll` (always 64-bit) - `atol`
   buys nothing over `atoll` in cflat, and this removes a platform-dependent type from
   the core API surface.

Option 2 is probably right: the whole point of the LLP64 comment is that `atol` is a
width trap, and cflat already exposes `atoll`/`strtoll` for the 64-bit case.
