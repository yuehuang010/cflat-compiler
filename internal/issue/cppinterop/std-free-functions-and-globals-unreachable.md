# Nothing in namespace `std` resolves except class templates

Found 2026-09-09 by the std header coverage spike ([`std-header-coverage-spike.md`](std-header-coverage-spike.md), gap 1).
Highest-leverage of the eight - it alone makes 21 `c*` headers plus `algorithm`, `numeric`, `bit`,
`limits`, `format` and the iostream family unusable.

## Repro

```cflat
import cpp "cstring";
extern int main() { if ((int)std.strlen("abcd") != 4) return 1; return 0; }
```

    cstring.cb(2,29): Undefined variable std.

Same for every `std::` free function and `std::` global tried: `std.abs` (`cstdlib`), `std.sqrt`
(`cmath`), `std.printf` (`cstdio`), `std.time` (`ctime`), `std.toupper` (`cctype`),
`std.numeric_limits<int>.max()` (`limits`), `std.popcount` (`bit`), `std.max` (`algorithm`, both
deduced and with an explicit `<int>`), `std.gcd` (`numeric`), `std.cout` (`iostream`).
`std.format` reports the different message `'format' is not a member of namespace 'std'`.

## Scope - what still works

- The GLOBAL-namespace spelling of the same C function binds: `strlen("ab")` after
  `import cpp "cstring"` passes.
- Namespaced free functions from a USER header bind (`cppi.make_tracked`,
  `Test/test_cpp_interop.cb`), so namespace-qualified calls are not broken in general.
- `std` CLASS TEMPLATES bind (`std.vector<int>`, `std.string`, `std.map<int,int>`).

So the failure is specific to non-class members of `std`.

## Root cause

Not established. The two things to check first: whether the declaration harvest that registers a
C++ namespace's free functions runs at all for a system header import, and whether the MSVC STL's
inline-namespace / `_STD_BEGIN` spelling of `std` defeats the namespace match that user headers
pass.

Secondary defect either way: the diagnostic. `Undefined variable std.` does not say the name was
looked up in a C++ namespace and not found there, which is what sent the spike down the wrong path
twice.

## Fix direction

Register `std` free functions and globals on the same path that already registers a user header's
namespaced free functions, and give the failure a diagnostic that names the namespace and the
member.

Acceptance: the repro above compiles and runs to exit 0; `std.sqrt`, `std.max` and one `std`
global (`std.cout`, which also needs
[`stream-classes-no-callable-destructor.md`](stream-classes-no-callable-destructor.md)) get
assertions in `Test/test_cpp_interop.cb`.
