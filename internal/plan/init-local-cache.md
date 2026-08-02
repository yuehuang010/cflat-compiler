# `--init-local` / `--init-clear-local`: place the compiler cache next to the executable

## Goal

Add a `--init-local` switch that populates the compiler cache in `<exeDir>/.cflat` instead of
`%USERPROFILE%\.cflat` / `~/.cflat`, and make every subsequent compile find that cache
automatically - without needing a flag on every invocation. Add a matching
`--init-clear-local` that deletes that local cache specifically.

Motivating cases: portable/xcopy installs, CI images where `$HOME` is not stable, multiple
cflat builds (Debug/Release, several git worktrees) that today all collide on one
per-user cache, and read-only or shared home directories.

## Current state

The cache root is computed in exactly one place:

- `cflat/LLVMBackend.cpp:2816` `LLVMBackend::GetCflatCacheDir()` - `USERPROFILE + "\\.cflat"` on
  Windows, `HOME + "/.cflat"` elsewhere, `{}` on failure. Declared `cflat/LLVMBackend.h:5657`.

Everything else derives from it, so one function is the whole lever:

| Derived path | Site |
|---|---|
| `<cache>/runtime/<coreHash>` core bitcode | `LLVMBackend.cpp:3995` `GetRuntimeBitcodeDir` |
| `<cache>\lib\<arch>` synthetic import libs | `LLVMBackend.cpp:3502` `GetSyntheticLibDir` |
| `<cache>/cheaders` C-header cache | `LLVMBackend.h:20420` `GetCHeaderCacheDir` |
| `<cache>/macsdk` libSystem stub | `LLVMBackend.cpp:3678` `MacStubSyslibroot` |
| `<cache>/compiler_path.txt` | `LLVMBackend.cpp:2986` (read by `vscode-extension/src/extension.ts:17,50`) |
| `<cache>\linker_paths_<arch>.json` | `LLVMBackend.cpp:3203` load, `:3237` save |

Consumers that read the cache on a *normal* compile (not just `--init`): core bitcode load
(`LLVMBackend.cpp:501`), linker-path cache load *and save* (`:3246`), synthetic import libs
(`LLVMBackend.h:8331`), macOS syslibroot (`LLVMBackend.h:8022`), C-header disk cache
(`LLVMBackend.h:7650` / `:7738`).

`--init` is registered at `cflat/main.cpp:321` and dispatched at `:361`; `--init-clear` at
`:322` / `:358`. Flags live in `ArgParser`'s flag map (`cflat/ArgParser.h:12`, `:162`) - no
struct field to add. The exe directory is already in hand at `main.cpp:349`
(`GetExeDir()` -> `LspServer.h:11` -> `PlatformCompat.h:104`).

## Design

### Resolution order (the core change)

Rewrite `GetCflatCacheDir()` as a resolver with a cached result:

1. **Process override** - set by `--init-local` for its own run.
2. **`CFLAT_CACHE_DIR` env var**, if set and non-empty. Escape hatch for CI and for
   worktree isolation without touching the exe tree.
3. **`<exeDir>/.cflat`, if it exists as a directory.** This is what makes `--init-local`
   sticky: once created, every later compile from that exe picks it up with no flag.
4. **`$HOME/.cflat` / `%USERPROFILE%\.cflat`** - today's behaviour, unchanged when no local
   cache exists.

Local wins over home deliberately: a cache sitting next to the exe is an explicit act by
whoever installed that exe, and it keeps two builds of cflat from sharing one cache.

`GetCflatCacheDir()` calls `GetExeDir()` itself, so no plumbing through call sites is needed.
It stays `static` and gains a function-local `static std::string` memo (resolve once per
process) plus a `SetCacheDirOverride()` used by `--init-local`. Note the memo must be set
before any consumer runs; the LSP path (`LspServer.cpp:1758`) is covered because resolution
is lazy and self-contained.

### `--init-local`

- `main.cpp:~322`: `args.addFlag("init-local", 0, "Populate <exe dir>/.cflat cache instead of the per-user cache, then exit")`.
- `main.cpp:~352`: generalize the mutual-exclusion check to reject any two of
  `init` / `init-local` / `init-clear` / `init-clear-local`. With four flags the current
  pairwise `if` is no longer the right shape: count how many of the four are set and error
  once with the list of names seen.
- `main.cpp:~361`: `--init-local` takes the same branch as `--init` (ftime-trace wiring +
  `RunInit(runtimeDir, verbose)`), after calling
  `LLVMBackend::SetCacheDirOverride(GetExeDir() + "/.cflat")`.
- Fail fast with a clear `LogError`-style message if the exe directory is not writable
  (probe by creating the directory; report the path and the OS error). Common on
  `/usr/local/bin`-style installs.

`RunInit` (`LLVMBackend.cpp:3820`) needs no logic change - it already creates the cache dir
at `:3829` and writes everything through the derived getters.

### `--init-clear` and `--init-clear-local`

Semantics:

- **`--init-clear`** clears **both** caches - the local `<exeDir>/.cflat` *and* the per-user
  `~/.cflat` / `%USERPROFILE%\.cflat`. "Clear the cache" means the next compile is genuinely
  cold, with no fallback tree left to silently take over.
- **`--init-clear-local`** clears **only** `<exeDir>/.cflat`, leaving the per-user cache
  intact. This is the surgical form: drop a portable install's cache without touching the
  shared per-user one.

This changes `--init-clear` from today's behaviour. It currently deletes whatever
`GetCflatCacheDir()` resolves to (`LLVMBackend.cpp:2841`), which is the per-user path today
but would become resolution-dependent - and therefore ambiguous - once a local cache can
exist. Deleting both removes that ambiguity.

Implementation:

- Refactor `RunInitClear` (`LLVMBackend.cpp:2839`) to take an **explicit root path** rather
  than calling `GetCflatCacheDir()` itself: `bool ClearCacheDir(const std::string& root, bool verbose)`.
  The resolver must not be involved here - both targets are named directly. Keep
  `RunInitClear(verbose)` as the two-target driver, or drop it and call the new function
  twice from `main.cpp`; either is fine, but the *path computation* moves out of the deleter.
- Per-user root: the existing `USERPROFILE`/`HOME` logic, which after the resolver rewrite
  lives in step 4 - factor it into a small `GetUserCacheDir()` so both the resolver and
  `--init-clear` can call it. Local root: `GetExeDir() + "/.cflat"`.
- `main.cpp:~323`: `args.addFlag("init-clear-local", 0, "Delete the <exe dir>/.cflat cache directory and exit; leaves the per-user cache alone")`,
  and update the `--init-clear` description to say it clears both.
- Dispatch: `--init-clear` calls the deleter for both roots (skipping a root that is empty or
  that the two resolve to the same path - possible if `HOME` points at the exe dir); it must
  attempt the second root even if the first fails, and exit non-zero if either failed.
  `--init-clear-local` calls it for the local root only.
- Print each target path and what happened to it ("deleted" / "not present"), in all four init
  flags. `--init-clear` prints one line per root.
- Not-found is success, not an error: clearing a cache that does not exist reports
  "not present" and exits 0. Check what `RunInitClear` does today on a missing root and make
  the behaviour explicit rather than incidental. In particular **`--init-clear-local` with no
  local cache is a no-op**: it prints "no local cache at <path>", touches nothing (it must not
  fall back to the per-user cache), and exits 0. Same for `--init-clear` when one of its two
  roots is absent - it clears the other and still exits 0.
- The safety guard at `LLVMBackend.cpp:2854` requires `filename() == ".cflat"`; a local root
  still satisfies it. Re-read the parent/root guards at `:2859` against an exe-dir path and
  confirm they still hold - an exe sitting directly under a filesystem root is the case to
  think about. These guards become more load-bearing now that the deleter takes a caller-
  supplied path, so keep them inside `ClearCacheDir` where every caller gets them.
- `CFLAT_CACHE_DIR` is deliberately **not** a `--init-clear` target: it is a per-invocation
  redirect, and deleting a directory the user pointed an env var at is not what "clear the
  cache" should mean. If it is set, print a note naming it so the user is not left thinking
  a stale cache was removed.

### Separator cleanup (in scope, low risk)

`LLVMBackend.cpp:3203`, `:3237`, `:3507` build paths with a hardcoded `\\`. These are
Windows-only code paths today, but a local cache makes them more likely to be exercised in
mixed contexts. Switch to `/` or `std::filesystem::path`. `GetCHeaderCacheDir`
(`LLVMBackend.h:20420`) already carries a comment about this pitfall.

## Open decision

**`compiler_path.txt` and the VS Code extension.** The extension discovers the compiler by
reading `~/.cflat/compiler_path.txt` (`vscode-extension/src/extension.ts:17,50`). With
`--init-local`, that file lands in the local cache and the extension will not see it.

Recommendation: have `--init-local` write `compiler_path.txt` to **both** the local cache and
the home directory (best-effort; ignore failure if home is unwritable). It is a single small
file, it is the one cache entry whose consumer lives outside the compiler, and doing so keeps
the extension working for the "portable cflat, normal home dir" case. Document the
best-effort behaviour in `doc/CACHING.md`. If the user prefers strict locality, drop this and
document that `--init-local` does not register the compiler with the VS Code extension.

## Implementation steps

1. `LLVMBackend.h` / `.cpp`: rewrite `GetCflatCacheDir()` as the 4-step resolver + memo; add
   `SetCacheDirOverride(std::string)`. Keep the existing doc comment style.
2. `main.cpp`: register `--init-local` and `--init-clear-local`, replace the pairwise
   exclusion check with an "at most one of four" check, wire the override into both local
   branches, add the writability probe and the "cache dir: <path>" line.
3. `LLVMBackend`: split `RunInitClear` into `ClearCacheDir(root, verbose)` (guards stay
   inside) plus a `GetUserCacheDir()` shared with the resolver; `--init-clear` drives both
   roots, `--init-clear-local` drives the local one. Print per-root status; missing root is a
   no-op with exit 0; verify the delete guards against an exe-dir path.
4. Separator cleanup at `LLVMBackend.cpp:3203, 3237, 3507`.
5. `compiler_path.txt` dual-write per the decision above.
6. Docs: `doc/CLI.md` (synopsis line 9, flag table near line 146), `doc/CACHING.md`
   (resolution order, layout block lines 49-60, `--init-clear` section 75-121, troubleshooting
   185-186), `CLAUDE.md` "Compiler cache (`--init`)" section (198-207), `README.md:9`.

## Verification

No new test files (per `CLAUDE.md`); `--init` has no dedicated test today - it is exercised as
a pre-step in `test.sh:14`, `test.bat:156`, `example.bat:379`.

- `./cmake_build.sh release`
- `x64/Release/cflat --init-local -v` -> creates `<exeDir>/.cflat`, prints the resolved path.
- Compile a test with `-v` and confirm it reports a core-bitcode cache hit from the *local*
  path, not `~/.cflat`.
- `./test.sh Release` -> full suite green with the local cache in place (this is the real
  regression signal: the whole suite runs warm off the cache).
- Temporarily move `<exeDir>/.cflat` aside and re-run `./test.sh Release` to confirm the home
  fallback is untouched.
- `CFLAT_CACHE_DIR=<scratch>/cache x64/Release/cflat --init -v` -> honours the env var.
- With **both** caches populated: `x64/Release/cflat --init-clear-local -v` -> deletes only
  `<exeDir>/.cflat`; `~/.cflat` is still on disk afterward and the next compile reports a
  cache hit from it.
- `x64/Release/cflat --init-clear-local` a second time -> "no local cache at <path>", exit 0,
  and `~/.cflat` still untouched (proves the no-op does not fall through to the per-user cache).
- With both caches populated: `x64/Release/cflat --init-clear -v` -> both roots gone; the next
  compile is genuinely cold (`-v` shows a core-bitcode cache miss and a reparse).
- `x64/Release/cflat --init-clear` with neither cache present -> two "not present" lines,
  exit 0.
- `--init --init-local`, `--init-clear --init`, `--init-local --init-clear-local` -> rejected
  with the combined-flag error.
