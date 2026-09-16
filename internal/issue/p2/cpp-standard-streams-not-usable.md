# C++ standard stream classes are not usable from CFlat

## Summary

`std::ostringstream` and `std::stringstream` cannot be used directly after importing
`<sstream>`. The familiar aliases are absent, the canonical class has no callable default
constructor, and an inline C++ bridge that actually uses the streams fails to link against
libc++ on arm64 macOS. This removes the normal C++ formatting and parsing path. Found
2026-09-16 during the standard-library dogfood session.

## Repro

```cflat
import cpp "sstream" cache;

extern int main()
{
    std.ostringstream output = default;
    output << "id=" << 42;
    return 0;
}
```

Observed diagnostics:

```
text.cb(13,22): cannot find the type 'std.ostringstream'
probe-stream-alias.cb(5,21): cannot find the type 'std.stringstream'
text.cb(13,34): C++ class 'std.basic_ostringstream$char' has no default constructor cflat can call - initialize it with 'std.basic_ostringstream$char(args)'
text.cb(11,20): error: found '<<' but expected ';'
```

The chained-shift parse error is already tracked by
`internal/issue/p2/shift-operator-does-not-chain.md` and is not duplicated here. A
scratch header using ordinary C++ `std::ostringstream` and `std::stringstream` bodies
compiled far enough to link, then failed with unresolved libc++ symbols including:

```
std::__1::ios_base::flags[abi:nqe220106]() const
std::__1::ios_base::width[abi:nqe220106]() const
std::__1::ios_base::setstate[abi:nqe220106](unsigned int)
```

The runnable workaround in `scratch/dogfood/std/text.cb` uses CFlat string concatenation
and C++ bridge calls for numeric conversion.

## Root cause (hypothesis)

The alias records are not published by the `<sstream>` catalog. The underlying stream
specialization is recognized, but its constructors are inherited through virtual-base
classes and are refused by the current ABI path. The unresolved symbols indicate that
the emitted inline stream bodies and the final macOS link are not using a compatible or
complete libc++ runtime surface. These paths were not traced to one definitive function.

## Fix direction

Bind the standard character-stream aliases to their canonical specializations, provide a
safe construction path for `basic_ostringstream` / `basic_stringstream`, and ensure the
macOS C++ companion link supplies the libc++ ABI symbols needed by inline stream bodies.
Add a small formatting and round-trip row to the interop fixture.
