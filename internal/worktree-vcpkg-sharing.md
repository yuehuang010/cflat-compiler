# Worktrees sharing vcpkg_installed

`vcpkg_installed` is ~26 GB and takes ~50 min to build. A naive `git worktree add`
makes each worktree build its own copy. These three repo-root helper scripts let
worktrees **share the main checkout's `vcpkg_installed`** via a directory junction.

The scripts are untracked (the no-commit rule); a project memory records their
existence so they can be recreated if lost.

## Scripts

| Script | Purpose |
|--------|---------|
| `new-worktree.bat <path> [-b branch]` | `git worktree add` + junction the worktree's `vcpkg_installed` at main's, then age manifest mtimes so no `vcpkg install` runs |
| `rm-worktree.bat <path>` | Unlink the junction with `rmdir` FIRST, then `git worktree remove` |

```bash
# create a worktree that shares the 26 GB deps
new-worktree.bat C:\source\cflat-feature -b feature/foo

# build it (no vcpkg rebuild)
msbuild C:\source\cflat-feature\cflat.slnx -p:Configuration=Release -p:Platform=x64

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

## Why the junction works

Everything keys off the MSBuild property `VcpkgManifestRoot`:

- `vcpkg.props` sets it to the dir above `vcpkg.json` (the worktree root), **only
  if unset** (`Condition="'$(VcpkgManifestRoot)' == ''"`).
- The vcpkg install dir defaults to `$(VcpkgManifestRoot)\vcpkg_installed\$(VcpkgTriplet)\`.
- `cflat.vcxproj` *also hardcodes* `$(VcpkgManifestRoot)\vcpkg_installed\...` for the
  antlr4 include path (lines ~139/163) and the lld-link/clang-cl copy step (lines ~623-627).

The junction keeps `VcpkgManifestRoot` = worktree, so all of those resolve through
`<worktree>\vcpkg_installed` -> (junction) -> `<main>\vcpkg_installed`. No source change needed.

## The rebuild trap (cost real time once)

A fresh `git worktree add` writes `vcpkg.json` with **mtime = now**, newer than the
shared MSBuild stamp (`vcpkg_installed\<triplet>\.msbuildstamp-...`). That defeats the
stamp-skip, so the worktree's first build invokes `vcpkg install`.

The **main repo normally never runs `vcpkg install`** - it coasts on the stamp-skip.
So if the installed tree is ABI-stale versus the current MSVC toolset (e.g. after a VS
upgrade to VS18 / MSVC 14.51), that re-resolve decides packages need rebuilding
(antlr4, llvm, zlib, zstd) and rebuilds them **straight into the shared dir** - removing
the old LLVM first, so the main repo is also broken until it finishes.

`new-worktree.bat` prevents this by aging the worktree's `vcpkg.json` /
`vcpkg-configuration.json` mtimes back to main's, so the worktree also benefits from the
stamp-skip and never invokes vcpkg. The optional snapshot (above) is the only fallback.

## Deletion hazard

`git worktree remove` and `rmdir` treat a junction correctly (unlink only, target safe).
But **MSYS `rm -rf` and Explorer can follow the junction and delete the shared 26 GB.**
Always remove worktrees with `rm-worktree.bat` (it `rmdir`s the junction first).
