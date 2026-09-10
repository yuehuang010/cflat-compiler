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

`open` is how a stream is pointed at a file once gap 1 (`std.cout` and free functions) and the
constructor rung are out of the way. On its own it does not block anything today, because
[`cpp-virtual-base-constructor-unreachable.md`](cpp-virtual-base-constructor-unreachable.md)
already keeps a stream out of a local.

## Notes toward a root cause

The three emptied bodies are related: `basic_ios::narrow` picks up a default-argument wrapper,
that wrapper's instantiation errors, `ErrorReachScan` then reaches every body that calls it, and
`open` is one of them. So `open` is likely a VICTIM of the `narrow` default wrapper rather than
ill-formed itself. Start by finding the diagnostic clang actually raised inside
`__cflat_dflt__narrow_...` - the extraction consumer swallows it, so it has to be captured
deliberately.

## Fix direction

Unknown until the swallowed diagnostic is read. If `narrow`'s default wrapper is the cause, the
existing `droppedCxxDefaultWrappers` path should already contain the damage - check why the drop
does not stop `ErrorReachScan` from poisoning `open` as well.

Acceptance: `std.basic_ofstream$char.open` binds under `--check -v` for the `const char*`
overload, and, once the constructor rung is closed, a `std.ofstream` local opens a file in
`scratch/`, writes and destructs.
