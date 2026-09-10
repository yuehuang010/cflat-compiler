# C++ class templates named only through an alias do not resolve

Found 2026-09-09 by the std header coverage spike ([`std-header-coverage-spike.md`](std-header-coverage-spike.md), gap 2).

## Repro

```cflat
import cpp "fstream";
extern int main() { std.ofstream f = default; return 0; }
```

    fstream.cb(2,33): cannot find the type 'std.ofstream'

`std.ostringstream` (`sstream`) behaves the same. Both are alias declarations
(`using ofstream = basic_ofstream<char>;`), not classes.

Spelling the underlying specialization gets past this error and lands on
[`stream-classes-no-callable-destructor.md`](stream-classes-no-callable-destructor.md), so the two
must be fixed together before any stream type is usable.

## Root cause

Not established. The type lookup appears to consider only class/specialization declarations, so a
`TypedefNameDecl` whose underlying type is a registrable specialization is never followed. Note
`Test/test_cpp_interop.cb` section M21 already covers "lazy aliases", so some alias handling
exists - find out why the std stream aliases miss it.

## Fix direction

Resolve a C++ alias to its underlying type at the point the name is looked up, then register the
specialization it names.

Acceptance: `std.ofstream` and `std.ostringstream` resolve to the same registration as
`std.basic_ofstream<char>` / `std.basic_ostringstream<char>`, with an assertion in
`Test/test_cpp_interop.cb`.
