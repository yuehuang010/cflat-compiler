# macOS arm64 native build (internals)

Moved from CLAUDE.md. Validated on Apple Silicon. The quick invocation lives in
CLAUDE.md; this file is the "why it works" reference. See also
`internal/plan/cross-platform-macos.md` for the staged port status and
`internal/worktree-vcpkg-sharing.md` for the shared `vcpkg_installed` tree.

## Build + link path

The `macos-arm64-{debug,release}` presets (vcpkg `arm64-osx` triplet) build cflat
natively. `cflat foo.cb -o foo` defaults to `--platform macos` on a Darwin host,
emits a Mach-O arm64 object (versioned triple `arm64-apple-macosx11.0.0` so it
carries `LC_BUILD_VERSION`), and links via the **bundled `ld64.lld`** deployed next
to the exe (CMake copies it from the vcpkg LLVM). `EmitExecutableMachO` invokes it
directly with `-arch arm64 -platform_version macos 11.0.0 <ver> -syslibroot <root>
-lSystem`, mirroring the Windows `lld-link` path.

## Self-contained (no Xcode / Command Line Tools)

Self-contained after a one-time `cflat --init`: on Darwin, `--init` harvests
libSystem's reexport-closure symbols from the live dyld shared cache (export-trie
walk over the `/usr/lib/system/*` images) into a flattened tbd stub at
`~/.cflat/macsdk/usr/lib/libSystem.tbd`, and the link uses that as `-syslibroot`.

Fallbacks preserve robustness:
- No harvested stub -> `$SDKROOT`/`xcrun` SDK.
- No bundled `ld64.lld` -> host `clang` driver.

compiler-rt builtins resolve via libSystem's reexported `libcompiler_rt.dylib`, plus
`libclang_rt.osx.a` when a clang toolchain is present.

## Why `vcpkg.json` lists `llvm` with `target-aarch64` + `enable-rtti` + `lld`

RTTI is required: ANTLR's C++ runtime uses `dynamic_cast`, and a no-RTTI LLVM leaves
clang/llvm base-class typeinfo undefined at link. `lld` supplies the bundled
`ld64.lld`.

## One-time Mac toolchain

`brew install cmake ninja openjdk antlr pkg-config coreutils` (openjdk keg-only at
`/opt/homebrew/opt/openjdk/bin`; `gtimeout` from coreutils is what `test.sh` uses),
then bootstrap vcpkg and `vcpkg install --triplet=arm64-osx`. Build with Homebrew
tools + `openjdk` on PATH.

`cmake_build.sh` is the Mac/Linux counterpart to `cmake_build.bat` (`debug` |
`release`, picks the preset from `uname`). The raw `cmake --preset
macos-arm64-release` still works if `VCPKG_ROOT` and `java` are already on PATH. The
macOS preset points `VCPKG_INSTALLED_DIR` at a **shared tree outside the source dir**
(`~/.cflat-compiler-deps/vcpkg_installed`, override with `CFLAT_VCPKG_INSTALLED`).

## Darwin runtime specifics

Live in `core/os.posix.cb` behind `if const (__MACOS__)` (mmap `MAP_ANON`,
`CLOCK_MONOTONIC`, pthread struct sizes + explicit init since zeroed mutex/cond are
invalid on Darwin, `gettid` via `pthread_threadid_np`) and `core/thread.cb` (timed
join emulated with a completion flag - macOS has no `pthread_timedjoin_np`). termios
raw mode (72-byte Darwin struct) and the BSD socket layouts (sockaddr_in
sin_len/sin_family, addrinfo ai_addr@32, SOL_SOCKET/SO_REUSEADDR = Winsock/BSD values)
are also ported via `if const (__MACOS__)`.

`--run` (in-process ORC/LLJIT) works on arm64/Mach-O: `JitRun` forces emulated TLS
(native Mach-O TLV descriptors are not bootstrapped in the bare LLJIT), gates the
COFF-only object-linking-layer setup + SEH plugin to Windows, and marks the JIT module
`no-builtins` so LLVM's `_FORTIFY` libcall fold does not rewrite `__vsnprintf_chk` into
cflat's own `vsnprintf` (which would recurse infinitely).
