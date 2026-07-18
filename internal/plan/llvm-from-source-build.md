# Building LLVM from source (statically linkable, find_package-consumable)

## Objective

Learn to build LLVM/Clang/LLD from source so cflat can consume it as a set of
**static archives** (no `libLLVM` dylib/DLL), discoverable via `find_package(LLVM
CONFIG)` / `find_package(Clang CONFIG)` - replacing the vcpkg-provided LLVM. This
doc captures the *build recipe and consumption contract*. The C++ API porting work
(18 -> 21) is a separate effort; breaking changes are expected and out of scope here.

## Current state (the "from")

- **Version in use: LLVM 18.1.6.**
  - macOS/Windows: vcpkg, pinned by registry baseline `544a4c5c...` in
    `vcpkg-configuration.json`. `vcpkg.json` requests `llvm` with features
    `target-aarch64`, `enable-rtti`, `lld`.
  - Linux: apt `/usr/lib/llvm-18` (shared `libLLVM` dylib), not vcpkg.
- **How consumed** (`CMakeLists.txt`):
  - `find_package(LLVM CONFIG REQUIRED)` + `find_package(Clang CONFIG REQUIRED)`.
  - Two link strategies:
    - Non-Windows *and* `TARGET LLVM` exists (apt/vcpkg dylib) -> link the single
      shared `LLVM` target (`set(LLVM_LIBS LLVM)`), to avoid double-registering
      `cl::opt` CommandLine options.
    - Windows (static) -> `llvm_map_components_to_libnames(...)` with the explicit
      component list + explicit `AArch64*` libs.
  - Clang libs linked explicitly (`clangFrontend ... clangBasic`, ~18 libs).
  - Tools copied next to the exe from `${LLVM_TOOLS_BINARY_DIR}`: Windows
    `lld-link.exe`, `clang-cl.exe`; macOS `ld64.lld`.
  - **cflat requires RTTI** (enforced at `CMakeLists.txt:217-226`); LLVM must be
    built `LLVM_ENABLE_RTTI=ON`.

## Target (the "to")

- **LLVM 21.1.8** (latest stable, released 2025-12-16). LLVM 22 is trunk-only until
  ~Aug 2026 - do not target it for a reproducible build.
- Static component archives (`.a` / `.lib`), RTTI on, `clang` + `lld` projects,
  `X86` + `AArch64` targets, self-contained (no external `zlib`/`zstd`/`libxml2`
  dylib deps that would break static consumption).

## What "statically linked" means here (important clarification)

Two distinct things; keep them separate:

1. **Static *LLVM* linkage into cflat** - the achievable, in-scope goal. Build LLVM
   as per-component archives (`LLVM_LINK_LLVM_DYLIB=OFF`, the default) and link them
   into `cflat`. This is exactly what the Windows path already does today. Moving
   macOS/Linux onto source-built static archives makes all three platforms use the
   `llvm_map_components_to_libnames` branch, not the `TARGET LLVM` dylib branch.

2. **Fully static *executable*** (no libc/libSystem dylib) - NOT achievable on
   macOS (Apple ships no static libSystem; a fully static Mach-O is unsupported).
   On Linux it is possible with a musl/static-libstdc++ toolchain but is a separate,
   larger effort. Do not conflate the two. The realistic deliverable is #1.

## Getting the source

Pick one:

```bash
# Shallow clone of the exact release tag (recommended; ~1-2 GB)
git clone --depth 1 --branch llvmorg-21.1.8 \
  https://github.com/llvm/llvm-project.git

# OR the release source tarball (no git history)
#   https://github.com/llvm/llvm-project/releases/download/llvmorg-21.1.8/llvm-project-21.1.8.src.tar.xz
```

The CMake entry point is the `llvm/` subdir (`llvm-project/llvm/CMakeLists.txt`),
with sibling projects (`clang/`, `lld/`) enabled via `LLVM_ENABLE_PROJECTS`.

## The build recipe

```bash
SRC=$PWD/llvm-project
BUILD=$PWD/llvm-build            # native fs, NOT under the cflat source tree
INSTALL=$HOME/.cflat-compiler-deps/llvm-21.1.8   # sibling of the vcpkg tree

cmake -G Ninja -S "$SRC/llvm" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$INSTALL" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
  -DLLVM_ENABLE_RTTI=ON \
  -DLLVM_ENABLE_EH=ON \
  -DLLVM_ENABLE_ASSERTIONS=OFF \
  -DLLVM_BUILD_LLVM_DYLIB=OFF \
  -DLLVM_LINK_LLVM_DYLIB=OFF \
  -DLLVM_INCLUDE_TESTS=OFF \
  -DLLVM_INCLUDE_BENCHMARKS=OFF \
  -DLLVM_INCLUDE_EXAMPLES=OFF \
  -DLLVM_INCLUDE_DOCS=OFF \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DLLVM_ENABLE_ZSTD=OFF \
  -DLLVM_ENABLE_LIBXML2=OFF \
  -DLLVM_ENABLE_TERMINFO=OFF

cmake --build "$BUILD"
cmake --install "$BUILD"
```

### Flag rationale (why each one matters for cflat)

| Flag | Reason |
|------|--------|
| `LLVM_ENABLE_PROJECTS="clang;lld"` | cflat links ~18 `clang*` libs (C-interop auto-extern) and ships `lld-link`/`ld64.lld`/`clang-cl`. Both must be in the same tree so their CMake config packages install together. |
| `LLVM_TARGETS_TO_BUILD="X86;AArch64"` | Matches the vcpkg `target-aarch64` feature + native X86. cflat calls `InitializeAllTargets()` and links explicit `AArch64*` libs. Omitting a target = missing archive at link time. Add more only if cross-target codegen is needed. |
| `LLVM_ENABLE_RTTI=ON` | Hard requirement - `CMakeLists.txt` warns/errors otherwise. cflat uses `dynamic_cast`/`typeid` against LLVM types. |
| `LLVM_ENABLE_EH=ON` | cflat is built with exceptions; keep LLVM's EH setting consistent to avoid unwind-table mismatches. (RTTI+EH commonly paired.) |
| `LLVM_BUILD_LLVM_DYLIB=OFF` + `LLVM_LINK_LLVM_DYLIB=OFF` | **The static switch.** Produces per-component `.a`/`.lib` and makes `find_package` *not* define a `TARGET LLVM` dylib, so cflat takes the `llvm_map_components_to_libnames` path uniformly. |
| `LLVM_ENABLE_ZLIB/ZSTD/LIBXML2/TERMINFO=OFF` | Removes transitive external-dylib deps that would otherwise leak into the static consumer's link line and re-introduce dynamic dependencies. Turn a specific one back ON only if a needed feature requires it, and then prefer a static build of that dep. |
| `LLVM_INCLUDE_TESTS/BENCHMARKS/EXAMPLES/DOCS=OFF` | Cuts build time/size substantially; none are consumed. |
| `LLVM_ENABLE_ASSERTIONS` | OFF for the fast consumer build. Build a second `-DLLVM_ENABLE_ASSERTIONS=ON` install if you want LLVM's internal asserts to fire through cflat's `CompilerManager` crash handler during debugging. |

### Platform specifics

- **macOS (arm64)**: host compiler = Homebrew clang or Apple clang. Add
  `-DLLVM_HOST_TRIPLE=arm64-apple-darwin` only if cross-issues appear (usually
  auto-detected). The lld multicall binary installs `ld64.lld` (+ `lld-link`,
  `ld.lld`) as symlinks in `<install>/bin`; clang installs a `clang-cl` symlink -
  so `LLVM_TOOLS_BINARY_DIR` (= `<install>/bin`) satisfies cflat's tool-copy step.
- **Windows**: match the vcpkg `x64-windows-static` runtime with
  `-DLLVM_USE_CRT_RELEASE=MT` (and `MTd` for a debug build) so the CRT linkage
  agrees with cflat's `/MT`. Build from an `x64 Native Tools` shell (MSVC).
- **Linux**: this replaces apt `/usr/lib/llvm-18`. Build with clang + `-DLLVM_USE_LINKER=lld`
  (or `mold`) for a much faster link. A static source build removes the current
  apt-vs-vcpkg version split.

### Build cost (rough, order-of-magnitude)

- Time: ~30-90 min on 8-16 cores (Release, tests off). Link is the long tail -
  use `lld`/`mold` and, for iteration, `-DLLVM_PARALLEL_LINK_JOBS=2` to bound RAM.
- Build dir: ~20-40 GB. Install tree: ~2-4 GB (static archives dominate).
- `ccache` (`-DLLVM_CCACHE_BUILD=ON`) makes rebuilds after a version bump cheap.

## Consuming the result in cflat

The install tree is self-describing:

```
<install>/lib/cmake/llvm/LLVMConfig.cmake     -> find_package(LLVM CONFIG)
<install>/lib/cmake/clang/ClangConfig.cmake   -> find_package(Clang CONFIG)
<install>/lib/cmake/lld/LLDConfig.cmake        (if lld libs are needed directly)
<install>/bin/{clang-cl, lld-link, ld64.lld}   -> tool-copy source
<install>/include, <install>/lib/*.a
```

Point cflat's configure at it instead of the vcpkg toolchain:

```bash
cmake --preset <preset> -DCMAKE_PREFIX_PATH="$INSTALL"
#   or the narrower: -DLLVM_DIR=$INSTALL/lib/cmake/llvm -DClang_DIR=$INSTALL/lib/cmake/clang
```

### Consumer-side changes required (noted, not yet done)

1. **CMakeLists.txt link branch**: with a static build there is no `TARGET LLVM`,
   so macOS/Linux fall through to the Windows-style
   `llvm_map_components_to_libnames` branch automatically. Verify the existing
   component list + explicit `AArch64*` libs are complete on those platforms
   (the shared-dylib branch never exercised them). Watch for the CommandLine
   double-registration issue the dylib branch was avoiding - with a single static
   link of each component it should not recur, but confirm `--help`/`cl::opt`
   at runtime.
2. **CMakePresets.json**: drop the vcpkg `CMAKE_TOOLCHAIN_FILE`/`VCPKG_INSTALLED_DIR`
   for LLVM and set `CMAKE_PREFIX_PATH`/`LLVM_DIR`/`Clang_DIR` to `$INSTALL`.
   antlr4 / nlohmann-json / simdjson can stay on vcpkg (independent of LLVM) OR
   also move - out of scope here.
3. **The 18 -> 21 API port** (separate task): highest-churn areas identified are
   ORC/LLJIT/JITLink (`ExecutionEngine/Orc/*`, `JITLink/*`), `TargetMachine`
   construction/`CodeGenOpt` enums, pass/instrumentation constructors
   (`AddressSanitizer.h`), and the Clang frontend driver in `CClangExtract.cpp`.
   Opaque pointers are ALREADY migrated and the code is on the new PassManager, so
   the single biggest historical churn source is a non-issue.

## Open questions / to validate on a real build

- Does the static component list fully satisfy the macOS/Linux link (it was only
  ever proven on Windows)? Enumerate missing libs from the first failed link.
- Does turning `zlib`/`zstd` OFF break any Object/Bitcode path cflat exercises?
  (Compression is used for some debug/section handling - likely fine, verify.)
- Confirm `<install>/bin` contains the `clang-cl` and `lld-link` symlinks on
  macOS/Linux (multicall lld + clang driver names) for the tool-copy step.
- Decide where the install tree lives for worktree sharing: mirror the vcpkg
  convention (`~/.cflat-compiler-deps/llvm-<ver>`, override env var) so worktrees
  share one ~3 GB tree instead of rebuilding.

## Validated build (2026-07-17, macOS arm64, this worktree)

Ran the full recipe end-to-end and verified the output is `find_package`-consumable
and statically linkable. Concrete results:

- **Source**: `git clone --depth 1 --branch llvmorg-21.1.8` = 2.4 GB checkout, ~16 s.
- **Configure**: clean. One benign warning: `LLVM_ENABLE_TERMINFO` is an unused
  variable (that knob was removed upstream; terminfo defaults off). `llvm-mt` was
  skipped (libxml2 off, as intended). LibEdit probed and not found (fine).
- **Build+install**: 4363 ninja targets, `-j18` on an 18-core/48 GB box. Install
  tree = **2.4 GB**, **160 static `.a` archives** under `lib/`, CMake config
  packages for all three of **llvm / clang / lld**, and `bin/` contains
  `clang-cl`, `lld-link`, `ld64.lld`, `clang` (the tool-copy source is satisfied).
- **Not a monolithic dylib**: no `libLLVM.dylib`. Four convenience shared libs ARE
  still emitted by default - `libclang.dylib`, `libclang-cpp.dylib`, `libLTO.dylib`,
  `libRemarks.dylib` - but cflat links none of them (it links the static component
  archives). Optionally suppress with `-DCLANG_LINK_CLANG_DYLIB=OFF`
  `-DLLVM_BUILD_LLVM_DYLIB=OFF` (latter already set); not required for correctness.
- **Consumer test** (`~/llvm21-work/verify-consume`): a standalone CMake project =
  `find_package(LLVM/Clang/LLD CONFIG)` + `llvm_map_components_to_libnames` +
  link `clangBasic`, building a program that emits IR. Result: builds, runs, prints
  the module and `CONSUMABLE-OK`; `otool -L` shows **no LLVM/clang dynamic dep** =
  fully static LLVM linkage. `LLVM_PACKAGE_VERSION=21.1.8`, `LLVM_ENABLE_RTTI=ON`,
  `LLD_FOUND=1` all confirmed from the installed config.

### Gotchas learned (feed into the cflat consumer migration)

1. **Consumer must enable the C language.** `LLVMConfig.cmake` includes
   `FindLibEdit.cmake`, which runs a C `check_include_file`; a `project(x CXX)`-only
   consumer errors with *"Unknown extension .c ... try_compile works only for
   enabled languages"*. cflat's `CMakeLists.txt` already enables C (for `.c`
   interop) so it is unaffected - but any minimal consumer needs `project(x C CXX)`.
2. **`InitializeAllTargets()` requires linking EVERY built target's libs.** The
   install built `X86;AArch64`, so `InitializeAllTargets/AllTargetMCs` reference
   both `LLVMInitializeX86*` and `LLVMInitializeAArch64*`. Linking only the
   `AArch64*` libs (as cflat's static branch does today) leaves X86 init symbols
   unresolved on macOS/Linux. **Action for cflat**: in the
   `llvm_map_components_to_libnames` branch, add the `X86CodeGen X86AsmParser
   X86Desc X86Info` libs alongside the existing `AArch64*` list (Windows already
   gets X86 via `native`/`nativecodegen` = host; macOS host is AArch64 so X86 must
   be explicit). Alternatively switch to `InitializeNativeTarget()` if cross-target
   codegen is never needed - but cflat currently calls `InitializeAllTargets()`.
3. **Duplicate-library link warning is benign.** `llvm_map_components_to_libnames`
   plus explicit target libs produces overlapping entries; the linker warns
   *"ignoring duplicate libraries"* and proceeds. Harmless.
4. **No `ccache` on this box** - a version re-bump would rebuild from scratch.
   Install `ccache` + `-DLLVM_CCACHE_BUILD=ON` before iterating on versions.

## References

- Releases: https://github.com/llvm/llvm-project/releases (llvmorg-21.1.8)
- Building LLVM with CMake: https://llvm.org/docs/CMake.html
- Advanced build configs (distributions/static): https://llvm.org/docs/AdvancedBuilds.html
