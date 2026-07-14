# Shared out-of-tree vcpkg_installed (Windows + macOS)

`vcpkg_installed` is ~26 GB (Windows) / 12 GB (macOS). Both platforms keep this tree
**outside the source dir**, in one fixed per-user location, so every worktree reads
the same copy and a naive `git worktree add` cannot make it materialize a duplicate:

- Windows: `%USERPROFILE%\.cflat-compiler-deps\vcpkg_installed`
- macOS: `~/.cflat-compiler-deps/vcpkg_installed`

Override on either platform with `CFLAT_VCPKG_INSTALLED`. The win-x64 and macos-arm64
presets in `CMakePresets.json` point `VCPKG_INSTALLED_DIR` at this path, with
`VCPKG_MANIFEST_INSTALL=OFF` - so CMake never runs `vcpkg install` itself, it only
consumes whatever is already sitting there.

Because nothing lives inside the source tree, a plain `git worktree add` / `git
worktree remove` just works on both platforms - no junction, no symlink, no helper
scripts, no mtime aging, and **no deletion hazard** (a worktree contains nothing that
points at the shared deps, so even `rm -rf` on a worktree cannot reach them):

```bash
git worktree add ../cflat-feature -b feature/foo
cd ../cflat-feature && ./cmake_build.sh release   # macOS/Linux; cmake_build.bat on Windows
git worktree remove ../cflat-feature               # shared tree untouched
```

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
`vcpkg.json` features, bumping the registry baseline, or upgrading cmake/clang. The
proof is sitting in the cache - two 2.6 GB LLVM archives differing by exactly one
token (`enable-rtti`), i.e. one feature edit cost one full rebuild. This is why
CLAUDE.md forbids touching `vcpkg.json` without permission.

## Populating the tree on a fresh clone

Neither `cmake_build.bat` nor `cmake_build.sh` populates the tree - they only build
against it (`VCPKG_MANIFEST_INSTALL=OFF`), and they error out with a pointer to the
right command if it is missing.

**Windows**: run `vcpkg-build.bat` once. It installs from `vcpkg.json` into
`%CFLAT_VCPKG_INSTALLED%` (`x64-windows-static` triplet, `x64-windows` host triplet,
`--clean-after-build` to keep the tree from ballooning past 200 GB), landing packages
at `<tree>\x64-windows-static\`.

**macOS**: there is no dedicated provisioning script. If `cmake_build.sh` finds the
tree missing, it prints the exact `vcpkg install --triplet=arm64-osx ...` line to run
against the main checkout's `VCPKG_ROOT`.

## Stale build dir after the tree moves

`cmake_build.bat` and `cmake_build.sh` both compare the `VCPKG_INSTALLED_DIR` cached
in `build/<preset>/CMakeCache.txt` against the currently resolved path, and wipe that
build dir if they differ. This matters because a build dir configured against a
different dependency tree keeps stale absolute paths (`LLVM_DIR` etc.) that a plain
reconfigure would silently reuse instead of pointing at the moved tree.
