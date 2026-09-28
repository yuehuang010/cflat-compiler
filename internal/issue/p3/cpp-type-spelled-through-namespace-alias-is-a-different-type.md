# A C++ class named through a namespace alias is not the same CFlat type as its real spelling

`namespace ondemand = arm64::ondemand;` (simdjson) - declaring a local as
`simdjson.ondemand.parser` gives an object whose member calls produce types that do not match the
real spelling `simdjson.arm64.ondemand.*`. Spelled through the real namespace, the same program
compiles. Found 2026-09-26 validating the simdjson spike: `scratch/simdjson_spike/o6.cb`, `o7.cb`,
`o8.cb` (all built and ran on 2026-09-08) now fail with:

```
cannot initialize C++ class 'simdjson.simdjson_result<simdjson.arm64.ondemand.document>' from this
expression; use '...(args)', '= default', a '...' lvalue, or 'move <...> lvalue'
```

on `simdjson.simdjson_result<simdjson.arm64.ondemand.document> doc = parser.iterate(view);` when
`parser` is declared `simdjson.ondemand.parser`. Declared `simdjson.arm64.ondemand.parser`, the
identical line compiles and runs (`value=42`). Not the cache (fresh `CFLAT_CACHE_DIR` same result),
not incremental-specific (`CFLAT_CPP_INCREMENTAL=0` same), same with Homebrew and vcpkg simdjson.

## Repro (in-repo, ~1 s)

```cpp
// na.h
#pragma once
#include <utility>
namespace na {
template <class T> struct Result {
    T first; int err = 0;
    Result() = default;
    Result(T&& v) : first(std::move(v)) {}
    int error() const { return err; }
};
namespace impl {
struct Doc { int v = 0; Doc() = default; explicit Doc(int x) : v(x) {}
             Doc(const Doc&) = delete; Doc& operator=(const Doc&) = delete;
             Doc(Doc&&) = default; Doc& operator=(Doc&&) = default; };
struct Parser { Parser() = default; Result<Doc> iterate(int n) { return Result<Doc>(Doc(n)); } };
}
namespace alias = impl;
}
```

```cflat
import cpp "na.h";
extern int main()
{
    na.alias.Parser p = default;            // na.impl.Parser here -> compiles, runs 0
    na.Result<na.impl.Doc> r = p.iterate(42);
    return r.first.v == 42 ? 0 : 1;
}
```

Via the alias: `na_al.cb(5,31): Unknown identifier 'iterate'.` - the in-repo shape fails earlier
(member lookup) than simdjson (result type), but both say the alias spelling produces a distinct
type instead of the aliased class.

## Fix direction

Canonicalize a C++ record reached through a namespace alias to its real qualified name at type
resolution (both `ParseDeclarationSpecifiers` copies), so `na.alias.Parser` and `na.impl.Parser`
are one CFlat type. Existing alias support to extend: 6e48e5b4 ("hop a C++ namespace alias declared
inside ..."), ec40e9cc (binds C++ namespace aliases). Check the `--init` cache round-trip if a new
field is needed.

## Notes for the fixing agent

- Regression: add the `na.h` shape to a `Test/library/cpp_interop_*.h` fixture and legs to
  `Test/test_cpp_interop.cb` covering declare-via-alias, call a member, and initialize a local of
  the REAL spelling from the result (and the reverse). No new test files.
- Real-world check (local-only, skip if `scratch/` absent), from the repo root:
  `x64/Release/cflat scratch/simdjson_spike/o6.cb --c-include /opt/homebrew/opt/simdjson/include --c-lib /opt/homebrew/opt/simdjson/lib/libsimdjson.dylib -o scratch/validate_0926/o6 && ./scratch/validate_0926/o6`.
  o6/o7/o8 also spell `simdjson_result<long>` for `get_int64()`; on macOS `int64_t` is `long long`,
  so that line needs `simdjson_result<i64>` under the declared-identity rule (6953f4d4) - edit a
  scratch copy, that part is not this bug.
- simdjson headers are also in the shared dep tree:
  `~/.cflat-compiler-deps/vcpkg_installed/arm64-osx/include` + `lib/libsimdjson.a`.
