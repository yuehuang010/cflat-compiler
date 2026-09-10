# Stream classes have no cflat-callable destructor, so they cannot be locals

Found 2026-09-09 by the std header coverage spike ([`std-header-coverage-spike.md`](std-header-coverage-spike.md), gap 3).

## Repro

```cflat
import cpp "fstream";
extern int main() { std.basic_ofstream<char> f = default; return 0; }
```

    cannot declare a local of C++ class 'std.basic_ofstream$char': it has no destructor cflat can
    call (the destructor is implicit or defined inline in the header) - hold it through a pointer
    instead

`std.basic_ostringstream<char>` identical.

## Impact

Combined with
[`std-free-functions-and-globals-unreachable.md`](std-free-functions-and-globals-unreachable.md)
(which keeps `std.cout` out of reach) and
[`cpp-alias-template-types-unresolvable.md`](cpp-alias-template-types-unresolvable.md) (which keeps
the `ofstream` spelling out of reach), NO part of `iostream` / `fstream` / `sstream` / `ostream` /
`istream` / `streambuf` / `iomanip` / `syncstream` is usable from CFlat today. That is 9 of the
105 headers, and the ones a newcomer reaches for first.

## Root cause

The message states it: the destructor is implicit or inline-only, so no out-of-line symbol exists
to call. Section M7 of `Test/test_cpp_interop.cb` proves cflat CAN make clang emit definitions for
header-only code, so the question is why the stream hierarchy's implicit destructor is not
requested the same way - most likely because it is implicit rather than merely inline, and nothing
asks clang to synthesize it.

## Fix direction

Ask clang to define the implicit destructor (and any other implicit special member a declared
local needs) the way M7 already forces inline definitions to be emitted. If that turns out to be
infeasible for the stream hierarchy specifically, rule iostreams out explicitly and say so in the
diagnostic instead of pointing at a pointer workaround that gap 1 makes useless.

Acceptance: a `std.ofstream` local opens a file in `scratch/`, writes, and destructs at scope
exit, asserted in `Test/test_cpp_interop.cb`.
