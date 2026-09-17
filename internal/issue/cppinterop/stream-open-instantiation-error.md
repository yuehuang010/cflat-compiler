# basic_ofstream<char>::open cannot be instantiated - clang errors inside the body it generated

Found 2026-09-10 while landing the virtual-member thunk (see
[`std-header-coverage-spike.md`](std-header-coverage-spike.md) gap 3). Measured, not root-caused.

## Repro

```cflat
import cpp "fstream";
extern int main() { std.basic_ofstream<char> f = default; return 0; }
```

`--check -v`:

    C++ member std.basic_ofstream$char.open not bound: cannot be instantiated for these template
    arguments (clang reported an error inside the body it generated)

and, upstream of it in the same run:

    C++ body emptied, its instantiation reported an error: std::basic_ofstream<char>::open
    C++ body emptied, its instantiation reported an error: __cflat_use_ctori0_0
    C++ body emptied, its instantiation reported an error:
        __cflat_dflt__narrow___basic_ios_DU__char_traits_D_std___std__QEBADDD_Z_2

The two `char*`/`wchar_t*` overloads of `open` are bound; only the `basic_string` ones are lost,
and those are separately refused for `takes unsupported type 'const std::basic_string<char> &'`.

## Impact

`open` is how a stream is pointed at a file once gap 1 (`std.cout` and free functions) is out of
the way. The constructor rung LANDED 2026-09-16 (virtual-base constructors bind through a
placement-new thunk), and on macOS arm64 / libc++ `open` is NOT reproduced any more: an
`std.ofstream` local opens, writes and closes. Keep this open for the MSVC STL, which is what it
was filed against.

## Notes toward a root cause

The three emptied bodies co-occurred: `basic_ios::narrow` picked up a default-argument wrapper,
that wrapper's instantiation reported an error, and `ErrorReachScan` reached bodies that call it,
including `open`. These observations do not establish whether `open` is ill-formed or only
affected by the wrapper failure. Start by finding the diagnostic clang actually raised inside
`__cflat_dflt__narrow_...` - the extraction consumer swallows it, so it has to be captured
deliberately.

## Fix direction

Unknown until the swallowed diagnostic is read. If `narrow`'s default wrapper is the cause, the
existing `droppedCxxDefaultWrappers` path should already contain the damage - check why the drop
does not stop `ErrorReachScan` from poisoning `open` as well.

Acceptance: `std.basic_ofstream$char.open` binds under `--check -v` for the `const char*`
overload, and, once the constructor rung is closed, a `std.ofstream` local opens a file in
`scratch/`, writes and destructs.
