# Worktrees sharing vcpkg_installed

`vcpkg_installed` is 12 GB (macOS) / ~26 GB (Windows). A naive `git worktree add`
makes each worktree materialize its own copy. The two platforms solve this
differently, because only Windows still pins the tree inside the source dir.

## What actually costs 50 minutes (read this first)

A worktree **cannot** trigger an LLVM source rebuild. vcpkg's binary cache
(`~/.cache/vcpkg/archives`) is **user-global, not repo-local**, and its key is an
ABI hash over the inputs in `<VCPKG_ROOT>/buildtrees/llvm/<triplet>.vcpkg_abi_info.txt`:

- the port's patches + `portfile.cmake` (pinned by the baseline in `vcpkg-configuration.json`)
- the **feature list from `vcpkg.json`**
- the triplet, and `triplet_abi` (a hash of the host compiler + flags)
- the **CMake version** (literally an ABI input)

The source directory is not among them. So a fresh worktree that runs
`vcpkg install` gets a cache *hit* and unzips the 2.6 GB archive - minutes, not
the ~50 min of compiling.

What does mint a new ABI hash, and therefore a real source build: editing
`vcpkg.json` features, bumping the registry baseline, or upgrading cmake/clang.
The proof is sitting in the cache - two 2.6 GB LLVM archives differing by exactly
one token (`enable-rtti`), i.e. one feature edit cost one full rebuild. This is why
CLAUDE.md forbids touching `vcpkg.json` without permission.

## macOS: no scripts needed

The macOS preset points `VCPKG_INSTALLED_DIR` at `~/.cflat-compiler-deps/vcpkg_installed`
(override with `CFLAT_VCPKG_INSTALLED`) - **outside any source tree**. Every worktree
reads that one copy, so `git worktree add` needs no post-processing at all:

```bash
git worktree add ../cflat-feature -b feature/foo
cd ../cflat-feature && ./cmake_build.sh release   # ~270 MB of build output, no vcpkg step
git worktree remove ../cflat-feature
```

`cmake_build.sh` resolves `VCPKG_ROOT` from the **main** checkout (via
`git rev-parse --git-common-dir`), because `./vcpkg` is gitignored and so a linked
worktree has no clone of its own. It also wipes a build dir whose cached
`VCPKG_INSTALLED_DIR` no longer matches, since a moved dep tree leaves stale absolute
paths (`LLVM_DIR` etc.) in `CMakeCache.txt`.

No junction, no symlink, no mtime aging, and **no deletion hazard**: nothing inside a
worktree points at the deps, so `rm -rf` on a worktree cannot reach them.

## Windows: junction scripts

The Windows preset still pins `VCPKG_INSTALLED_DIR` to `${sourceDir}/vcpkg_installed`,
so worktrees share the main checkout's tree via a directory junction. The scripts are
untracked (the no-commit rule); a project memory records their existence so they can be
recreated if lost.

### Scripts

| Script | Purpose |
|--------|---------|
| `new-worktree.bat <path> [-b branch]` | `git worktree add` + junction the worktree's `vcpkg_installed` at main's, then age manifest mtimes so no `vcpkg install` runs |
| `rm-worktree.bat <path>` | Unlink the junction with `rmdir` FIRST, then `git worktree remove` |

```bash
# create a worktree that shares the 26 GB deps
new-worktree.bat C:\source\cflat-feature -b feature/foo

# build it (no vcpkg rebuild)
cd C:\source\cflat-feature && cmake_build.bat release

# remove it safely
rm-worktree.bat C:\source\cflat-feature
```

**Optional safety net.** The mtime alignment in `new-worktree.bat` is what prevents
rebuilds, so a standing backup is normally unnecessary. But before a risky operation
(e.g. a known MSVC/VS upgrade that will invalidate the ABI), you can snapshot the
known-good tree and restore it in ~2 min instead of waiting ~50 min for a rebuild:

```bash
robocopy <main>\vcpkg_installed C:\source\vcpkg_installed.bak /MIR /MT:16   # snapshot
robocopy C:\source\vcpkg_installed.bak <main>\vcpkg_installed /MIR /MT:16   # restore
```

### Why the junction works

The CMake build resolves vcpkg deps relative to the source dir: `cmake_build.bat`
sets `VCPKG_ROOT` and the Windows preset points `VCPKG_INSTALLED_DIR` at
`${sourceDir}/vcpkg_installed` (see `CMakePresets.json`) with
`VCPKG_MANIFEST_INSTALL=OFF`, so it reuses whatever is physically at
`<worktree>\vcpkg_installed` instead of triggering vcpkg's manifest-install step.

The junction keeps `<worktree>\vcpkg_installed` -> (junction) -> `<main>\vcpkg_installed`,
so the CMake configure finds the shared tree with no source change needed.

### The mtime rebuild trap (MSBuild-era; does not apply to the CMake flow)

This is why `new-worktree.bat` ages mtimes. It is a **legacy** concern: under the
current CMake presets `VCPKG_MANIFEST_INSTALL=OFF`, so CMake never invokes
`vcpkg install` at all and manifest mtimes are irrelevant. Kept here because the
scripts still do it and it explains the code.

A fresh `git worktree add` writes `vcpkg.json` with **mtime = now**, newer than the
shared build's stamp (`vcpkg_installed\<triplet>\.msbuildstamp-...` under the legacy
MSBuild flow this mechanism was originally built for). That defeats the
stamp-skip, so the worktree's first build invokes `vcpkg install`.

The **main repo normally never runs `vcpkg install`** - it coasts on the stamp-skip.
So if the installed tree is ABI-stale versus the current MSVC toolset (e.g. after a VS
upgrade to VS18 / MSVC 14.51), that re-resolve decides packages need rebuilding
(antlr4, llvm, zlib, zstd) and rebuilds them **straight into the shared dir** - removing
the old LLVM first, so the main repo is also broken until it finishes.

`new-worktree.bat` prevents this by aging the worktree's `vcpkg.json` /
`vcpkg-configuration.json` mtimes back to main's, so the worktree also benefits from the
stamp-skip and never invokes vcpkg. The optional snapshot (above) is the only fallback.

### Deletion hazard (Windows only)

`git worktree remove` and `rmdir` treat a junction correctly (unlink only, target safe).
But **MSYS `rm -rf` and Explorer can follow the junction and delete the shared 26 GB.**
Always remove worktrees with `rm-worktree.bat` (it `rmdir`s the junction first).
