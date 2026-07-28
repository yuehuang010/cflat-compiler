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
| Metadata schema changed | `version` in `core_*.meta.json` is bumped; an older version is rejected and the cache is rebuilt from source |
| `--init` re-run | Overwrites the files in the current hash directory |
| Cache absent or unreadable | Transparent fallback to full source parse |

## Clearing the cache (`--init-clear`)

`cflat --init-clear` is the inverse of `--init`: it deletes the whole cache directory and exits.

```bash
cflat.exe --init-clear
```

```
Cache directory: /Users/you/.cflat
  Removed 24 file(s) and 0 symlink(s) in 10 directory(ies), 18452193 bytes total.
Run 'cflat --init' to repopulate the cache (on macOS this also re-harvests the
libSystem link stub at macsdk/usr/lib/libSystem.tbd that self-contained linking needs).
```

Everything cached lives under the one root, so a single recursive delete covers the core
bitcode, `compiler_path.txt`, the linker-path JSON files, the synthesized import libs, the
`cheaders/` C-header cache, and the macOS `macsdk/` stubs. If the directory is already absent
it says so and exits 0.

Note that `cheaders/` is the one entry `--init` does **not** rebuild: the C-header cache is
populated lazily by compiles that use the `cache` import clause, so an expensive header cache
(e.g. `windows.h`) has to be re-earned by the next compile that imports it. Everything else in
the list comes back with one `--init`.

**Re-run `--init` afterward.** Normal compiles fall back to a full source parse when the
cache is missing, so nothing breaks - but on macOS the harvested libSystem stub at
`macsdk/usr/lib/libSystem.tbd` is what makes linking self-contained without Xcode /
Command Line Tools. Until `--init` re-harvests it, the `-o` link falls back to a
`$SDKROOT`/`xcrun` SDK, which may not be installed.

### What exactly gets deleted

Whatever `$HOME/.cflat` (`%USERPROFILE%\.cflat` on Windows) resolves to at the moment you run
the command is deleted recursively. The path is taken verbatim from the same helper `--init`
writes through - there is no canonicalisation, no ownership check, and no confirmation prompt.
That is the intended contract: it deletes exactly where `--init` put things, so pointing `HOME`
somewhere else points both commands at the same place. If a `HOME` you did not expect is in the
environment, this deletes that `.cflat` instead.

Two behaviours are worth knowing:

- If the cache root is itself a symlink, only the link is removed and the target tree is left
  untouched. Symlinks *inside* the tree are likewise removed as links, never followed.
- If part of the tree cannot be removed (permissions), the recursive delete still removes what
  it can, so the command reports that the cache is partially deleted, names the blocking path
  when the filesystem gave one, and exits 1. Re-run `--init` either way.

The code additionally refuses to act unless the resolved path's last component is exactly
`.cflat` and it has a parent directory. Those checks cannot fail as the code stands - the
cache-dir helper always appends `.cflat` - so they are defence-in-depth against a future change
to that helper, not a filter on hostile input.

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

## macOS

On macOS the cache directory is `~/.cflat` (no `%USERPROFILE%` env var). `cflat --init`
populates it with:

- `compiler_path.txt` - same role as on Windows: the VS Code extension reads this to
  auto-detect the compiler when `cflat.executablePath` is not set.
- `macsdk/usr/lib/libSystem.tbd` - a flattened link stub harvested from the live dyld
  shared cache (export-trie walk over `/usr/lib/system/*`), used as the link `-syslibroot`
  so linking is self-contained without Xcode / Command Line Tools.
- `runtime/<hash>/core_macos.bc` - the core bitcode cache, same purpose and invalidation
  rule as `core_win64.bc` on Windows (hash derived from core `.cb` file mtimes).

There is no per-arch linker-path cache on macOS (the bundled `ld64.lld` is invoked directly,
not discovered via a VS/SDK scan), so that section of the Windows cache does not apply.

## Troubleshooting

- **Cache not taking effect**: run `cflat.exe --init` to rebuild it after an update.
- **Unexpected errors after update**: run `cflat.exe --init-clear` to delete `%USERPROFILE%\.cflat\`, then re-run `--init`.
- **Bypass for debugging**: pass `--no-cache` to force a full parse for a single invocation.
- **Profiling a slow compile**: pass `-ftime-trace` (clang's single-dash spelling) to write a Chrome-trace JSON to `<input>.time-trace.json`. Load it in `chrome://tracing` or Perfetto to see where the time goes. With a warm cache `RuntimeImport` should be small; the usual remaining cost is `CHeaderExtract` (libclang parsing of any imported C header, e.g. `windows.h`), which is not yet disk-cached.
