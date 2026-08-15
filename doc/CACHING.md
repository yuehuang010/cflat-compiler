# Compiler Cache

`cflat --init` pre-builds a persistent cache that eliminates two cold-start costs paid on
every invocation: linker path discovery and core library parsing.

## Cache directory resolution

Every compile (not just `--init`) resolves the cache directory the same way, in this order,
resolved once per process:

1. **Process override** - set internally by `--init-local` for its own run only.
2. **`CFLAT_CACHE_DIR`** environment variable, if set and non-empty. An escape hatch for CI
   images with an unstable `$HOME`, or for pointing a worktree at its own cache without
   touching the exe tree. Not touched by `--init-clear` / `--init-clear-local` - see below.
3. **`<exe dir>/.cflat`**, if it already exists as a directory **and looks populated** - it
   must contain both `compiler_path.txt` and a `runtime/` directory. This is what makes
   `--init-local` sticky: once created, every later compile from that same exe picks it up
   automatically, with no flag needed. The populated check keeps an empty or half-written
   local cache (an interrupted or permission-failed `--init-local`) from hijacking a good
   per-user cache; such a directory falls through to step 4. Run a compile with `-v` to see
   which root was resolved and which rule matched.
4. **The per-user cache** - `%USERPROFILE%\.cflat\` on Windows, `~/.cflat` on macOS/Linux.
   Today's behaviour, unchanged when no local cache exists.

`--init` populates whichever directory this resolves to. `--init-local` forces step 3 by
creating `<exe dir>/.cflat` first (so step 3 then applies to every later compile too).

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
%USERPROFILE%\.cflat\        (or <exe dir>\.cflat with --init-local)
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

## `--init-local`: a cache next to the executable

```bash
cflat.exe --init-local
```

Populates `<exe dir>/.cflat` instead of the per-user cache, then exits. Once that directory
exists, resolution step 3 above picks it up on every later compile from that same exe - no
flag needed on the compile itself. Useful for:

- portable/xcopy installs where there is no stable per-user home to write into,
- CI images where `$HOME` is not guaranteed to persist between runs,
- keeping several cflat builds (Debug/Release, multiple git worktrees) from colliding on one
  shared per-user cache.

`--init-local` fails fast (prints the path and the OS error, exits 1) if the exe directory is
not writable - common for a `/usr/local/bin`-style install.

**VS Code extension caveat.** The extension only reads `~/.cflat/compiler_path.txt` to
auto-detect the compiler. `--init-local` best-effort seeds `compiler_path.txt` into the
per-user cache dir too (in addition to the local one), so the extension keeps working for the
common case of "portable cflat, normal home directory."

That seed happens **only when the per-user record is absent or stale** - stale meaning the file
is missing, unreadable, blank, or names a path that no longer exists on disk. A record naming a
compiler that is still there is left untouched: it is a deliberate registration, and silently
repointing it at a worktree or portable build is exactly the collision `--init-local` exists to
prevent - `test.sh`, `test.bat`, and `example.bat` run `--init-local` on every suite run, so
without that rule a test run inside a worktree would hijack the extension's compiler.

The staleness half matters just as much: a record pointing into a worktree that has since been
removed would otherwise be unrepairable, because once a local cache exists plain `--init`
resolves to it too and would skip the per-user write forever. The seed is best-effort in the
other direction as well: if the home directory is not writable, `--init-local` still succeeds
and simply does not register with the extension.

## Cache invalidation

| Change | Effect |
|--------|--------|
| Core `.cb` file modified | New hash directory; old cache ignored |
| cflat.exe rebuilt | Core files unchanged -> same hash; cache still valid |
| Metadata schema changed | `version` in `core_*.meta.json` is bumped; an older version is rejected and the cache is rebuilt from source |
| `--init` re-run | Overwrites the files in the current hash directory |
| Cache absent or unreadable | Transparent fallback to full source parse |

## Clearing the cache (`--init-clear` / `--init-clear-local`)

**Behaviour change:** `--init-clear` now clears **both** the per-user cache and the local
(`<exe dir>/.cflat`) cache, not just whichever one the old single-root resolver happened to
pick. Once a local cache can exist, "clear the cache" deleting only one of the two roots would
leave the other silently taking over on the next compile - deleting both removes that
ambiguity. Use `--init-clear-local` for the surgical form that touches only the local cache.

```bash
cflat.exe --init-clear
```

```
cache dir: /Users/you/project/.cflat
local cache: /Users/you/project/.cflat
  Deleted 24 file(s) and 0 symlink(s) in 10 directory(ies), 18452193 bytes total.
cache dir: /Users/you/.cflat
per-user cache: /Users/you/.cflat
  Deleted 24 file(s) and 0 symlink(s) in 10 directory(ies), 18452193 bytes total.
```

```bash
cflat.exe --init-clear-local
```

```
cache dir: /Users/you/project/.cflat
local cache: /Users/you/project/.cflat
  Deleted 24 file(s) and 0 symlink(s) in 10 directory(ies), 18452193 bytes total.
```

Everything cached lives under one root, so a single recursive delete per root covers the core
bitcode, `compiler_path.txt`, the linker-path JSON files, the synthesized import libs, the
`cheaders/` C-header cache, and the macOS `macsdk/` stubs.

Note that `cheaders/` is the one entry `--init` does **not** rebuild: the C-header cache is
populated lazily by compiles that use the `cache` import clause, so an expensive header cache
(e.g. `windows.h`) has to be re-earned by the next compile that imports it. Everything else in
the list comes back with one `--init` / `--init-local`.

**Re-run `--init` / `--init-local` afterward.** Normal compiles fall back to a full source
parse when the cache is missing, so nothing breaks - but on macOS the harvested libSystem and
libobjc stubs under `macsdk/usr/lib/` are what make linking self-contained without Xcode /
Command Line Tools. Until it is re-harvested, the `-o` link falls back to a `$SDKROOT`/`xcrun`
SDK, which may not be installed.

### What exactly gets deleted

`--init-clear` deletes two roots directly, by name - the per-user cache (`GetUserCacheDir()`:
`$HOME/.cflat` / `%USERPROFILE%\.cflat`) and the local cache (`<exe dir>/.cflat`) - never the
general 4-step resolver from "Cache directory resolution" above. That is deliberate: with a
local cache able to exist, resolving a single "the" cache directory to delete would be
ambiguous, so both commands name their exact targets instead of asking the resolver to guess.

- A root that does not exist is reported ("not present" for `--init-clear`, "no local cache at
  \<path\>" for `--init-clear-local`) and is not an error; it does not fall back to touching
  the other root. `--init-clear` still attempts (and reports on) the second root even if the
  first is missing or failed.
- If the two roots happen to resolve to the same path (e.g. `HOME` points at the exe dir),
  `--init-clear` clears it once, not twice.
- `CFLAT_CACHE_DIR`, if set, is **not** a delete target for either command - it is a
  per-invocation redirect, not something "clear the cache" should reach into. Both commands
  print a note naming it instead, so you are not left thinking a stale cache was removed.
- If a cache root is itself a symlink, only the link is removed and the target tree is left
  untouched. Symlinks *inside* the tree are likewise removed as links, never followed.
- If part of a tree cannot be removed (permissions), the recursive delete still removes what
  it can, so the command reports that root as partially deleted, names the blocking path when
  the filesystem gave one, and exits 1 (the other root is still attempted). Re-run `--init` /
  `--init-local` either way.

The code additionally refuses to act on a root unless its last path component is exactly
`.cflat` and it has a parent directory. Those checks cannot fail for a root produced by
`GetUserCacheDir()` or `<exe dir>/.cflat` as the code stands today - they are defence-in-depth
against a future change to either helper, not a filter on hostile input.

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
- `macsdk/usr/lib/libSystem.tbd` and `libobjc.tbd` - flattened link stubs harvested from the
  live dyld shared cache (an export-trie walk over `/usr/lib/system/*` for libSystem, and over
  the dlopen'd `/usr/lib/libobjc.A.dylib` for libobjc), used by the link `-syslibroot` so
  linking is self-contained without Xcode / Command Line Tools.
- `runtime/<hash>/core_macos.bc` - the core bitcode cache, same purpose and invalidation
  rule as `core_win64.bc` on Windows (hash derived from core `.cb` file mtimes).

There is no per-arch linker-path cache on macOS (the bundled `ld64.lld` is invoked directly,
not discovered via a VS/SDK scan), so that section of the Windows cache does not apply.

## Troubleshooting

- **Cache not taking effect**: run `cflat.exe --init` (or `--init-local`) to rebuild it after an update.
- **Unexpected errors after update**: run `cflat.exe --init-clear` to delete both the per-user
  and local caches, then re-run `--init` (or `--init-local`).
- **Wondering which cache a compile is using**: pass `-v` and look for
  `[verbose] cache dir: <path> (rule: override|CFLAT_CACHE_DIR|local|per-user)`, followed by
  `[verbose] core bitcode cache: hit/miss`. A populated `<exe dir>/.cflat` from an old
  `--init-local` run takes priority over the per-user cache even if you did not pass a flag.
- **Bypass for debugging**: pass `--no-cache` to force a full parse for a single invocation.
- **Profiling a slow compile**: pass `-ftime-trace` (clang's single-dash spelling) to write a Chrome-trace JSON to `<input>.time-trace.json`. Load it in `chrome://tracing` or Perfetto to see where the time goes. With a warm cache `RuntimeImport` should be small; the usual remaining cost is `CHeaderExtract` (libclang parsing of any imported C header, e.g. `windows.h`), which is not yet disk-cached.
