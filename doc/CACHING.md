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

## Troubleshooting

- **Cache not taking effect**: run `cflat.exe --init` to rebuild it after an update.
- **Unexpected errors after update**: delete `%USERPROFILE%\.cflat\` and re-run `--init`.
- **Bypass for debugging**: pass `--no-cache` to force a full parse for a single invocation.
- **Profiling a slow compile**: pass `-ftime-trace` (clang's single-dash spelling) to write a Chrome-trace JSON to `<input>.time-trace.json`. Load it in `chrome://tracing` or Perfetto to see where the time goes. With a warm cache `RuntimeImport` should be small; the usual remaining cost is `CHeaderExtract` (libclang parsing of any imported C header, e.g. `windows.h`), which is not yet disk-cached.
