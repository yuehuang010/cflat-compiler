# Compiler Cache

`cflat --init` pre-builds a persistent cache under `%USERPROFILE%\.cflat\` that eliminates
two cold-start costs paid on every invocation: linker path discovery and core library parsing.

## Quick start

```bash
cflat.exe --init
```

Run once after installing or updating cflat. Output:

```
Cache directory: C:\Users\you\.cflat
  Saved compiler_path.txt
Discovering linker paths for x64...
  Saved linker_paths_x64.json
Discovering linker paths for x86...
  Saved linker_paths_x86.json
Building core bitcode cache for win64...
  Saved core_win64.bc + .meta.json
```

Re-run `--init` whenever you update cflat. Normal compiles detect staleness automatically
and fall back to a full parse if the cache is missing or out of date - no manual cleanup
required.

## What gets cached

### Compiler path

The full path of the `cflat.exe` that ran `--init` is recorded in `compiler_path.txt`. The
VS Code extension reads this file to auto-detect the compiler when `cflat.executablePath` is
not set, so a fresh install only needs one `--init` to wire up the language server. An explicit
`cflat.executablePath` setting always overrides the recorded path.

### Linker paths

Resolved paths for `lld-link.exe`, the MSVC toolchain lib dir, the UCRT lib dir, and the
Windows UM lib dir. Without the cache these are discovered on every compile by scanning
the VS and Windows SDK installation trees. The cached paths are loaded in microseconds.

### Core library bitcode

The compiled IR for all 20 core `.cb` libraries is stored as a single LLVM bitcode file.
Loading from cache is ~44% faster than parsing the libraries from source on every compile.

## Cache directory layout

```
%USERPROFILE%\.cflat\
  compiler_path.txt
  linker_paths_x64.json
  linker_paths_x86.json
  runtime\
    <hash>\
      core_win64.bc
      core_win64.meta.json
```

The hash is derived from the modification times of the core `.cb` files. Any change to a
core file produces a new hash and the old directory is ignored - it can be deleted safely.

## Cache invalidation

| Change | Effect |
|--------|--------|
| Core `.cb` file modified | New hash directory; old cache ignored |
| cflat.exe rebuilt | Core files unchanged -> same hash; cache still valid |
| `--init` re-run | Overwrites the files in the current hash directory |
| Cache absent or unreadable | Transparent fallback to full source parse |

## C-header cache (opt-in)

Binding a large C header (e.g. `import "windows.h";`) runs a full clang parse of the header
and its transitive includes. For `windows.h` that dominates a cold compile (~1.3s of ~2.3s,
visible as the `CHeaderExtract` phase under `-ftime-trace`). The in-memory cache only helps
*within* one process (LSP, `--check` batches); a fresh `cflat.exe` starts cold.

Opt a header into a persistent disk cache with the inline `cache` clause:

```cpp
import "windows.h" cache;
import package "curl/curl.h" lib "libcurl.lib" cache;   // clause comes last
```

The extracted declarations (functions, enums, records, macros, globals) are serialized to:

```
%USERPROFILE%\.cflat\cheaders\<key>.json
```

The `<key>` is an FNV-1a hash of the canonical header path plus every `--c-include` dir,
`--c-define`, and inline `define` - the same inputs as the in-memory cache key - so a header
exposed differently under different roots/defines never collides on a stale entry.

### Validation: shallow (default) vs deep

| Mode | Trigger | What is checked |
|------|---------|-----------------|
| Shallow | default | Top header mtime (hash on mtime drift) + the version-stamped SDK include dirs baked into the key. An SDK upgrade changes those paths -> automatic miss. |
| Deep | `--c-header-cache-deep` | Every transitively `#include`d file's mtime/hash, recorded as a `deps` list in the JSON and re-checked on load. Catches an in-place SDK header edit the top-header check would miss, at the cost of validating hundreds of files. |

A `cache` clause with no `--c-header-cache-deep` writes a shallow entry; adding the switch
rewrites it with a `deps` list on the next miss. Loading is forward-compatible: a shallow
entry skips the transitive check, a deep entry enforces it regardless of the current switch.

This is **distinct** from the `import package-vcpkg` cache, which is co-located in
`vcpkg_installed/.cflat-cache/` (a vcpkg package is already version-pinned, so it caches
unconditionally). Headers without a `cache` clause are never disk-cached.

## Troubleshooting

- **Cache not taking effect**: run `cflat.exe --init` to rebuild it after an update.
- **Unexpected errors after update**: delete `%USERPROFILE%\.cflat\` and re-run `--init`.
- **Bypass for debugging**: pass `--no-cache` to force a full parse for a single invocation.
- **Profiling a slow compile**: pass `-ftime-trace` (clang's single-dash spelling) to write a Chrome-trace JSON to `<input>.time-trace.json`. Load it in `chrome://tracing` or Perfetto to see where the time goes. With a warm cache `RuntimeImport` should be small; the usual remaining cost is `CHeaderExtract` (libclang parsing of any imported C header, e.g. `windows.h`), which is not yet disk-cached.
