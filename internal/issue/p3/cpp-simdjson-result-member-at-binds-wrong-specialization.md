# `simdjson_result<ondemand::document>::at(2)` binds the wrong member (result typed `dom.array`)

On a `simdjson.simdjson_result<simdjson.arm64.ondemand.document> doc`, the call `doc.at(2)`
resolves to something typed `simdjson.dom.array` - a class from the unrelated DOM API - so
`doc.at(2).get_int64()` fails:

```
C++ class 'simdjson.dom.array' has no member 'get_int64'.  Members of 'simdjson.dom.array': at, at_path, ...
```

Found 2026-09-26 validating the simdjson spike: `scratch/simdjson_spike/o3.cb`, `o4.cb` (built and
ran on 2026-09-08). In C++, `simdjson_result<ondemand::document>::at(size_t)` returns
`simdjson_result<ondemand::value>`. Not the cache (fresh `CFLAT_CACHE_DIR` same), not incremental-
specific (`CFLAT_CPP_INCREMENTAL=0` same).

## Repro (needs simdjson headers; in the shared dep tree, no install needed)

```cflat
import cpp "simdjson.h";
extern int printf(const char* fmt, ...);
extern int main()
{
    simdjson.arm64.ondemand.parser parser = default;
    simdjson.padded_string json = simdjson.padded_string("[1,2,42]", 8);
    simdjson.simdjson_result<simdjson.arm64.ondemand.document> doc = parser.iterate((simdjson.padded_string_view)json);
    simdjson.simdjson_result<i64> t = doc.at(2).get_int64();                 // FAILS: dom.array
    printf("v=%ld\n", t.value());
    return 0;
}
```

```bash
I=~/.cflat-compiler-deps/vcpkg_installed/arm64-osx      # or /opt/homebrew/opt/simdjson
x64/Release/cflat at_i.cb --c-include $I/include --c-lib $I/lib/libsimdjson.a -o at_i
```

Splitting the chain gives a DIFFERENT error, which suggests the overload set for `at` itself is
wrong, not just the chained member:

```cflat
simdjson.simdjson_result<simdjson.arm64.ondemand.value> e = doc.at(2);
// at_h.cb(8,64): cannot cast an aggregate value - a fixed array decays to a pointer to its first element
```

## Likely direction (unverified)

`simdjson_result<T>` is one class template with many specializations (dom::element, dom::array,
ondemand::document, ...). The member set bound for `simdjson_result<ondemand::document>` appears
to include `at` from another specialization or from an implicit conversion (`operator T`) target,
with a `dom::array`-returning or array-parameter signature. Dump the candidates for `at` on this
receiver (verbose overload trace, or `--out-lli` + the chosen callee name) before touching code.

## Notes for the fixing agent

- The first step is an in-repo reduction: a class template `R<T>` with an `at(size_t)` member in
  two specializations returning different types, plus an `operator T&()` conversion. Coverage has to
  land in `Test/library/cpp_interop_*.h` + `Test/test_cpp_interop.cb` (no third-party headers in
  tests); keep the simdjson program as the real-world check only.
- Use `simdjson_result<i64>`, not `<long>`: on macOS `int64_t` is `long long`, and since 6953f4d4
  CFlat spells primitives by declared identity. The spike's `<long>` spelling is stale, not this bug.
- Real-world check (local-only, skip if `scratch/` absent): o3.cb/o4.cb with `<long>` -> `<i64>`
  must print `error=0 value=42` and exit 0.
