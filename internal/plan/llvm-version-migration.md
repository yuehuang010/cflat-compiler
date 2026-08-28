# LLVM version migration: off vcpkg, 18 -> 22 (two phases)

Companion to [`llvm-from-source-build.md`](llvm-from-source-build.md), which owns
the *build recipe* for a source-built LLVM. This file owns the *migration plan*:
how cflat stops consuming vcpkg's LLVM (phase 1, version held constant) and then
moves to a modern LLVM (phase 2, code changes expected).

## Premise check (verified 2026-08-26)

- **vcpkg really is stuck at 18.** `ports/llvm/vcpkg.json` on vcpkg **master** is
  still `18.1.6` (port-version 6), not just our pinned baseline
  `544a4c5c...`. Bumping `vcpkg-configuration.json` buys nothing. The premise
  for leaving vcpkg is correct.
- **The llvm.org prebuilt tarballs cannot be used.** Official release binaries
  are built with the default `LLVM_ENABLE_RTTI=OFF`. cflat derives from
  polymorphic LLVM/Clang bases (`clang::ASTConsumer`, `ASTFrontendAction`,
  `llvm::DiagnosticHandler`, `orc::ObjectLinkingLayer::Plugin`), so a no-RTTI
  LLVM never emits the base typeinfo those derived classes need and the link
  fails. This is already documented at `CMakeLists.txt:302-320`; it is a hard
  requirement, not a preference. The Windows `LLVM-*-win64.exe` installer is
  additionally unusable because it ships no LLVM/Clang development libraries.
- **Prebuilt is still achievable per platform - just not from llvm.org.**
  Distributors that build with `LLVM_ENABLE_RTTI=ON`:
  | Platform | Prebuilt provider | RTTI | Link shape |
  |----------|-------------------|------|------------|
  | macOS arm64 | Homebrew `llvm@18` (18.1.8) / `llvm@22` (22.1.8) | ON | shared `libLLVM` dylib (`LLVM_LINK_LLVM_DYLIB=ON`) |
  | Linux | apt.llvm.org `llvm-18-dev` (already in use) / `llvm-22-dev` | ON | shared `libLLVM` |
  | Windows | **none** | - | source build required |

  So Windows is the only platform that forces a from-source LLVM. macOS and
  Linux can stay on bottled/packaged binaries for both phases.

## Phase 1 - swap the provider, hold the version (no compiler code changes)

Goal: cflat builds and `./test.sh` / `test.bat` pass against an LLVM that did not
come from vcpkg, at **18.1.x**, with zero changes under `cflat/`. This isolates
the CMake/consumer churn from the API churn.

Version drift 18.1.6 -> 18.1.8 is a patch bump and is expected to be code-neutral.

1. **macOS (do this first - cheapest, and it is the host in use).**
   - `brew install llvm@18`.
   - Preset `macos-arm64`: keep the vcpkg toolchain for antlr4 / nlohmann-json /
     simdjson, add `CMAKE_PREFIX_PATH=/opt/homebrew/opt/llvm@18` so
     `find_package(LLVM|Clang CONFIG)` resolves there instead of the vcpkg tree.
   - Remove the `llvm` entry (and its three features) from `vcpkg.json` **only
     after** Windows has a replacement - see the ordering note below.
   - Expect the link branch to flip: Homebrew defines `TARGET LLVM` (dylib), so
     macOS leaves the `llvm_map_components_to_libnames` branch and takes the
     `NOT WIN32 AND TARGET LLVM` branch that Linux already uses
     (`CMakeLists.txt:73-75`). That is the *easy* direction - it sidesteps the
     component-completeness question the from-source doc flags.
   - Verify the tool-deploy step still resolves: `ld64.lld` must exist in
     `${LLVM_TOOLS_BINARY_DIR}` (`/opt/homebrew/opt/llvm@18/bin`), because the
     self-contained Mach-O link depends on it (`CMakeLists.txt:400-407`).
   - Gate: `./cmake_build.sh release && ./test.sh Release` green (554/0).
2. **Linux.** Already on apt LLVM 18 and already off vcpkg. No phase-1 work
   beyond confirming `linux-x64` still configures once `vcpkg.json` drops llvm.
3. **Windows.** No RTTI prebuilt exists, so either:
   - (a) source-build 18.1.8 with the recipe in `llvm-from-source-build.md`
     plus `-DLLVM_USE_CRT_RELEASE=MT` / `MTd`, install to
     `%USERPROFILE%\.cflat-compiler-deps\llvm-18.1.8`; or
   - (b) **defer**: leave Windows on vcpkg 18.1.6 through phase 1 and move it
     straight to the phase-2 source build.
   (b) is recommended - a Windows source build of 18 is ~1h of machine time whose
   only product is a version cflat is about to abandon. It does mean `vcpkg.json`
   keeps the `llvm` dependency until phase 2; macOS just stops resolving it via
   `CMAKE_PREFIX_PATH` precedence rather than by removing the port.

## Phase 2 - move to LLVM 22 (code changes expected)

**Target 22.1.8, not 23.** LLVM 23.1.0 released 2026-08-25 (one day old);
22.1.x has had six months of point releases, is what Homebrew ships today
(`llvm` = 22.1.8, and a pinnable `llvm@22` already exists), and has apt.llvm.org
packages. Re-run this plan for 23 later if there is a reason to.

1. **Providers**: macOS `brew install llvm@22` (22.1.8, RTTI ON) - pin the
   **versioned** formula, never the unversioned `llvm`: the latter is a rolling
   major that Homebrew will bump to 23.1.x within weeks (23.1.0 released
   2026-08-25; as of 2026-08-26 `llvm` is still 22.1.8 and there is no `llvm@23`
   formula yet). Using `llvm` would silently move the macOS build to 23 on some
   future `brew upgrade`. Linux
   `llvm-22-dev` from apt.llvm.org; Windows source-build 22.1.8 with RTTI +
   `MT` CRT into `%USERPROFILE%\.cflat-compiler-deps\llvm-22.1.8`. Drop `llvm`
   from `vcpkg.json` here (antlr4 / nlohmann-json / simdjson stay on vcpkg).
2. **Consumer-side CMake** (`CMakeLists.txt`):
   - The Windows branch stays static (`llvm_map_components_to_libnames`); verify
     the component list against 22 - component names do move between releases.
   - Per `llvm-from-source-build.md` gotcha #2, a static build that includes both
     `X86` and `AArch64` must link `X86CodeGen X86AsmParser X86Desc X86Info`
     explicitly alongside the existing `AArch64*` list, because the code calls
     `InitializeAllTargets()`.
   - Windows tool-deploy assumes the vcpkg `bin/` + `debug/bin/` layout
     (`CMakeLists.txt:379-392`); a source-install tree has one `bin/`, so that
     path needs adjusting.
3. **API port, 18 -> 22.** Highest-churn areas for this codebase:
   - ORC / LLJIT / JITLink (`--run`, `ObjectLinkingLayer::Plugin`).
   - `TargetMachine` construction and the `CodeGenOpt` -> `CodeGenOptLevel`
     enum family.
   - Pass and instrumentation constructors (`AddressSanitizer.h`).
   - The clang driver/frontend use in `CClangExtract.cpp` (auto-extern AST dump).
   - Non-issues: opaque pointers are already migrated and the code is on the new
     PassManager - the two historically largest churn sources do not apply.
4. **Gate**: build + full suite green on the host doing the work, per platform.

## Ordering note

`vcpkg.json` is shared by all three platforms and must not be edited without
explicit permission (see `CLAUDE.md`). The `llvm` dependency therefore comes out
in one step at phase 2, once Windows has its source-built replacement. During
phase 1, macOS overrides the LLVM provider via `CMAKE_PREFIX_PATH` precedence in
its preset while the port stays declared.

## Open items

- Confirm Homebrew `llvm@18` ships the static `clang*.a` frontend archives cflat
  links (`clangFrontend ... clangSupport`, `CMakeLists.txt:255-259`) and not only
  `libclang-cpp.dylib`. If it ships the dylib only, link against `clang-cpp`
  instead of the 18-lib list on that platform.
- Homebrew builds with `LLVM_ENABLE_EH=OFF` (RTTI ON). apt LLVM is the same and
  Linux works today, so this is expected to be fine - but watch for unwind-table
  or `noexcept`-boundary surprises on the first macOS link.
- Decide whether the shared-dylib link on macOS is acceptable long term: it adds
  a runtime dependency on the Homebrew tree, which conflicts with the
  self-contained-toolchain goal in `internal/macos-build.md`. A source-built
  static LLVM (phase 2, or a later hardening step) removes that.

## Execution log

### Phase 1 done - macOS on Homebrew `llvm@18` (2026-08-26)

`brew install llvm@18` (18.1.8), `macos-arm64` preset gained
`LLVM_DIR` / `Clang_DIR` pointing at `/opt/homebrew/opt/llvm@18/lib/cmake/*`.
`vcpkg.json` untouched - CMake cache-variable precedence is enough to override
the provider, so Windows keeps resolving vcpkg's LLVM. Result: build clean,
`./test.sh Release` = **719 passed, 0 failed, 8 skipped**, zero changes under
`cflat/`. The open items from the phase-1 section resolved as follows:

- Homebrew `llvm@18` ships all 18 `clang*.a` frontend archives cflat links (90
  in total), plus `ld64.lld`, `lld-link` and `clang-cl` in `bin/`. RTTI **and**
  EH are ON (unlike the unversioned formula, which has EH OFF).
- macOS did flip to the `NOT WIN32 AND TARGET LLVM` shared-dylib branch, as
  predicted. No CommandLine double-registration, no missing components.
- **New problem, fixed:** the deployed `ld64.lld` links against
  `@rpath/libLLVM.dylib` with only `@loader_path/../lib` on its rpath, so
  copying it next to `cflat` broke every native link (`dyld: Library not
  loaded`). vcpkg's `ld64.lld` was static, so the copy step had never needed
  more than `cmake -E copy_if_different`. Deployment now goes through
  `cmake/deploy_macho_linker.sh`, which copies, adds the provider's lib dir to
  the rpath when the binary is dylib-linked, and re-signs adhoc (arm64
  invalidates the signature on any load-command edit). It is a plain copy for a
  static `ld64.lld`, so vcpkg and source builds are unaffected.

### Phase 2 in progress - macOS on `llvm@22` + `lld@22`

- **Homebrew split lld out of the llvm formula.** `llvm@22` has no `ld64.lld`,
  no `lld-link`, and no `lib/cmake/lld`; they live in the separate `lld@22`
  keg. `CMakeLists.txt` no longer assumes the Mach-O linker sits in
  `LLVM_TOOLS_BINARY_DIR`: it `find_program`s `ld64.lld` with
  `CFLAT_LLD_BIN_DIR` (set by the preset) as the first hint. vcpkg and source
  installs still satisfy the second hint.
- `llvm@22` is RTTI ON / EH **OFF** - same shape as apt LLVM, which Linux has
  used all along, so it is expected to be fine.
- First build against 22 produced 66 errors of exactly three kinds, all known
  upstream renames: `Module` triple is now `llvm::Triple` (21), 
  `Intrinsic::getDeclaration` -> `getOrInsertDeclaration` (20), and
  `jitlink::Symbol::getName()` now returns `orc::SymbolStringPtr` (21).
  Ported behind one `cflat/LLVMCompat.h` shim header, so the tree keeps building
  against 18 on Windows and Linux until those hosts move to 22.

**Phase 2 macOS result: `./test.sh Release` = 719 passed, 0 failed, 8 skipped on
LLVM 22.1.8 - identical to the LLVM 18 baseline. The same tree, reconfigured back
to `llvm@18`, also passes 719/0, so the dual-version claim is verified on both
sides, not just asserted.**

#### The API surface that actually moved (was shimmed in `LLVMCompat.h`)

> **Superseded 2026-08-27.** cflat targets LLVM 23 only; `cflat/LLVMCompat.h` has been
> deleted and every shim inlined at its call site. The two helpers that were not version
> dispatch - the null-tolerant `GetTerminatorOrNull` and the explicitly truncating
> `GetIntTruncated` - live in `cflat/LLVMBackend.h` under `namespace cflat_llvm`. The list
> below is kept as the record of what moved between 18 and 23.

| Change | Since | Shim |
|--------|-------|------|
| `Module` triple is `llvm::Triple`, not `std::string` | 21 | `SetModuleTriple` / `GetModuleTripleStr` |
| `Intrinsic::getDeclaration` -> `getOrInsertDeclaration` | 20 | `GetIntrinsicDecl` |
| `llvm.va_start`/`va_end` became overloaded on the va_list pointer type | 20 | `GetVaIntrinsicDecl` |
| Masked gather/load/store/scatter dropped the alignment operand | 22 | `CreateMasked*` |
| `jitlink::Symbol::getName()` returns `orc::SymbolStringPtr` | 21 | `JitLinkSymbolName` |
| `Target::createTargetMachine` takes a `Triple` | 21 | `CreateTargetMachine` |
| `RecordDecl::getBitWidthValue()` lost its context argument | 21 | `GetBitWidthValue` |
| clang `DiagnosticOptions` no longer ref-counted; `CompilerInstance` takes the invocation at construction | 21 | `ClangDiagnosticsState`, `CreateClangInvocation`, `CreateClangCompilerInstance` |

The `va_start` one is the trap worth remembering: passing the pointer type on 18
mangles `llvm.va_start.p0`, which release-18 does not recognize as an intrinsic,
so it silently becomes an external call and every native link fails with
`undefined symbol: _llvm.va_start.p0`. It compiles clean - only the link fails.

#### Three real defects that LLVM 22 exposed (not API churn)

These were latent bugs that LLVM 18's verifier tolerated. Each is fixed at its
source; none is a verifier workaround.

1. **`ResetForReanalysis` leaked module-bound state.** Seven containers keyed by
   `llvm::Value*`/`Function*` of the discarded module (`globalAssignBorrowOrigin_`,
   `rawArrayResults_`, `mayReachReturnInProgress_`, `joinAddressInProgress_`,
   `pendingReturnDangleChecks_`, `pendingNullIfaceDispatch_`,
   `pendingNullIfaceGlobal_`, plus the escape memo) survived the reset, against
   the invariant the surrounding comments state. Batch `--check` over the 322-file
   error suite reuses one process, so the stale pointers **segfaulted the
   compiler** mid-suite on 22.
2. **`expect_error` left half-formed globals in the module.** The rollback erased
   the struct shells a failed block created but not the globals typed by them, so
   a global kept an unsized initializer. LLVM 22 rejects that
   ("Global variable initializer must be sized"); 18 did not. The rollback in
   `MainListener_Declarations.cpp` now drops new globals whose value type is
   unsized, mirroring the existing shell rollback.
3. **ForwardRefScanner mangled a NESTED generic type argument by raw spelling.**
   `WcpGBox<WcpInner<int>>` registered its shell as `WcpGBox__WcpInner<int>` while
   the main pass instantiated `WcpGBox__WcpInner__int`, leaving the first opaque
   and passed by value. 22 rejects it ("Function arguments must have first-class
   types"). Nested generic args now route through `ResolveForwardTypeArg` like the
   closure and `unique` cases already did.

#### Note on the delegated attempt

The first delegated port reported success at "717 passed, remaining failures
environment-related". That was wrong: an independent run segfaulted in the
locale-discovery pass before the suite even started. The delta was ~200 lines of
verifier-bypass work - IR rewriting inside `VerifyModule` (replacing
cross-function operands with `undef`, rewriting bitcasts, clearing alignments
over 64, renaming functions to "refresh" intrinsic classification) plus
`isPointerTy()` guards that masked non-pointer storage at interface-boxing sites.
All of it was reverted; only the shim header and the three root-cause fixes above
were kept, and those alone reach 719/0. Anything that mutates IR inside
`VerifyModule` should be treated as a review defect on sight.

### Phase 2 hardening - static, source-built LLVM 22.1.8 (2026-08-26)

macOS no longer depends on Homebrew at all. `llvmorg-22.1.8` was built from
source with the recipe in `llvm-from-source-build.md` and installed to
`~/.cflat-compiler-deps/llvm-22.1.8`; the `macos-arm64` preset points
`LLVM_DIR` / `Clang_DIR` / `CFLAT_LLD_BIN_DIR` there. Result: `./test.sh Release`
= **719 passed, 0 failed, 8 skipped**, and `otool -L x64/Release/cflat` shows no
LLVM or clang dynamic dependency - fully static LLVM linkage, which is what the
vcpkg build gave us and the Homebrew build did not.

Measured on an 18-core / 48 GB Apple Silicon box:

| | |
|---|---|
| Shallow clone (`--depth 1 --branch llvmorg-22.1.8`) | 2.6 GB |
| Build (4473 ninja targets, `-j18`, `LLVM_PARALLEL_LINK_JOBS=4`) | under 20 min, 3.2 GB build dir |
| Install tree | 2.5 GB, 168 static archives, cmake packages for llvm + clang + lld |

Deltas from the recipe as written, all confirmed on this build:

- **Drop `LLVM_ENABLE_TERMINFO`** - removed upstream; passing it only produces an
  unused-variable warning.
- **`CLANG_LINK_CLANG_DYLIB=OFF` and `LLVM_BUILD_LLVM_C_DYLIB=OFF` do not
  suppress the convenience dylibs.** `libclang.dylib`, `libclang-cpp.dylib`,
  `libLTO.dylib` and `libRemarks.dylib` are still emitted. They are simply
  unused - cflat links the static archives and has no dynamic dependency on
  them - so this is cosmetic, not a correctness issue.
- **`lld` is in-tree**, so `bin/` holds `ld64.lld`, `lld-link` and `clang-cl`
  together and `CFLAT_LLD_BIN_DIR` is only pointing at the same directory as
  `LLVM_TOOLS_BINARY_DIR`. The Homebrew keg split is the reason that hint exists
  at all; a source build does not need it.
- **`ccache` bought nothing here** (0/3574 hits on a cold build, as expected) and
  is only worth having for a later re-bump or a config change. Its default 5 GB
  ceiling is too small for LLVM; raise it before relying on it.

**The predicted X86 link failure happened exactly as written in gotcha #2.**
Going static moves macOS off the `TARGET LLVM` dylib branch onto
`llvm_map_components_to_libnames`, and the first link failed with undefined
`LLVMInitializeX86AsmPrinter` / `Target` / `TargetInfo` / `TargetMC`, because
`native` resolves to AArch64 on this host while the code calls
`InitializeAllTargets()`. `CMakeLists.txt` now lists
`X86CodeGen X86AsmParser X86Disassembler X86Desc X86Info` alongside the
`AArch64*` libs. The duplicate-library link warning that follows is benign.

Windows can now follow the same path, which is its only route - no RTTI-enabled
prebuilt exists for it.

### Phase 2 done - Windows on static, source-built LLVM 22.1.8 (2026-08-26)

`bootstrap.bat` (new, repo root) provisions a clean clone end to end: toolchain
check -> vcpkg deps -> clone + source-build + install LLVM 22.1.8 -> build cflat
-> `--init-local` -> `test.bat Release`. Each step is skippable/idempotent.
Result on a 32-core box: `test.bat Release` **all passed, 0 skipped**,
`test_lsp.bat` all passed, `example.bat` **99 passed / 0 failed / 42 skipped**.
Install tree ~2.6 GB; the LLVM build is ~35 min, everything else is minutes.

Three Windows-specific findings, none of which macOS could have surfaced:

1. **`LLVM_USE_CRT_RELEASE=MT` no longer selects the CRT.** On 22 it only
   validates against `CMAKE_MSVC_RUNTIME_LIBRARY`, whose default is
   `MultiThreadedDLL`. Passing only the old knob yields a `/MD` LLVM that fails
   cflat's link with a wall of `msvcprt.lib ... LNK2005 already defined` plus
   `LNK4098 LIBCMT conflicts`. Pass
   `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`.
2. **`ObjectLinkingLayerCreator` lost its `Triple` parameter** (LLVM 21). The
   `--run` JITLink override in `LLVMBackend_EmitAndLink.cpp` now takes a trailing
   `auto&&...` pack so the lambda converts to either signature.
3. **`__ImageBase` is now pre-created as an EXTERNAL symbol** by the COFF
   LinkGraph builder, and it wins the lookup over the defined symbol
   `SehRegistrationPlugin` was adding - so it resolved to the host exe's image
   base and every JIT'd `.pdata` RVA overflowed its `Pointer32` fixup. `--run`
   was broken at *every* optimization level (only the `-O2` HPC checks exercise
   it, which is why exactly one test caught it). `FixImageBase` now
   `makeAbsolute`s that external symbol onto the lowest block. See
   `internal/run-jit-unwind.md`.

Only the `/MT` (Release) LLVM is built. cflat Debug is `/MTd` and cannot link it;
a second `MultiThreadedDebug` install is needed before `cmake_build.bat debug`
works on 22.
