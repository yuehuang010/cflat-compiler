# Plan: macOS framework linking (`import framework "AppKit";`)

Status: S1+S2 DONE (2026-07-08). S3 DONE (2026-07-08). S4 DONE (2026-07-08).
Created: 2026-07-08.

S4 landed: core/cocoa.cb migrated off dlopen onto `import framework { "AppKit",
"Foundation" };`. Every ui_native mac consumer (hello_objc, cocoa_probe, cocoa_window,
cocoa_native_settings, fedit, gallery, ui_native_cocoa) inherits the framework link
through cocoa.cb - no example dlopened a framework directly, so none needed a source
edit beyond stale-comment fixes. fn-ptr acquisition: extern-address casting of a
declared C function is UNSUPPORTED (`(function<...>)&objc_msgSend` -> "Unable to get an
Address-of an object without a Storage"), so the bridge keeps dlsym but now uses
dlsym(RTLD_DEFAULT=(void*)-2) instead of dlopen(AppKit)+dlsym(handle). Because the
frameworks are load commands, libobjc is dragged in at process load and RTLD_DEFAULT
resolves objc_msgSend/objc_getClass/... The typed objc_msgSend cast trick and every
msg*/addMethod* helper signature is UNCHANGED. Verified on arm64: otool -L on the built
gallery/fedit shows AppKit + Foundation as load commands (dlopen gone); test.sh 151/0,
example_mac.sh 33/0 (gallery/fedit/cocoa_settings/cocoa_probe/cocoa_window/framework_link
all PASS); --check --platform macos clean for cocoa.cb + ui_native_cocoa.cb. Docs:
doc/CLI.md (--framework + macOS frameworks section) and doc/LANGUAGE.md (macOS frameworks
import subsection) added.

S3 landed: `framework` clause on header/package/group imports (grammar frameworkClause +
DequoteFrameworkClauses wired at all 4 dispatch sites + if-const branch), BuildClangDriverArgs
macOS branch (`--target=arm64-apple-macosx11.0.0` + `-isysroot <real SDK>`; no SDK -> LogError,
LSP silent), MacSdkPathCached helper ($SDKROOT/xcrun, never the harvested stub root), framework
header resolution in ResolveImportPath (`Foo/Bar.h` -> <sdk>/System/Library/Frameworks/
Foo.framework/Headers/Bar.h, kept non-canonical so the extractor in-scope filter matches),
`.framework` bundle scoping in ExtractCHeaderClang (siblings spelled Headers/ vs real_path
Versions/A), and an extra link `-F <sdk>/System/Library/Frameworks` so frameworks absent from
the harvested root (e.g. CoreGraphics) resolve. Verified on arm64: example/macos/
framework_link.cb binds CoreGraphics with auto-extern (CGColorSpaceCreateDeviceRGB /
CGColorSpaceGetNumberOfComponents / CGColorSpaceRelease, NO hand externs) and runs exit 0
(link uses harvested macsdk syslibroot + SDK -F for CoreGraphics); test.sh 151/0, example_mac.sh
33/0. Known gap: opaque-struct-pointer typedef NAMES (`CGColorSpaceRef`) are not registered as
usable aliases on the Darwin header path - use the mapped `void*` (see internal/issue/).

S1+S2 landed: `import framework "X"` / `import framework { ... }` grammar + dispatch
(4 sites), `cFrameworks_` accumulator + `--framework` CLI flag, EmitExecutableMachO
`-framework`/`-F` on both link paths, and the --init harvest of AppKit/Foundation/
CoreFoundation/libobjc tbd stubs under macsdk/System/Library/Frameworks + usr/lib.
Verified on arm64: --init exit 0, example/macos/framework_link.cb links SDK-free via
bundled ld64.lld and runs (exit 0), test.sh 151/0, example_mac.sh 33/0, non-mac
target -> LogError exit 1.

## Motivation

The UI framework's Cocoa backend (core/ui_native_cocoa.cb + core/cocoa.cb) drives
AppKit through dlopen + dlsym + typed function<> casts of objc_msgSend because the
compiler has no way to link a Mach-O framework. This plan adds first-class framework
linking so Darwin programs can declare `import framework "AppKit";` and reference
framework symbols as plain externs. Pattern precedent: the `lib` clause -> cLinkLibs_
-> linker-args flow, and the vcpkg/nuget resolver shape (discovery only here - macOS
frameworks are on-machine, nothing is acquired).

Decisions (user, 2026-07-08):
- Syntax: `framework` clause family (NOT a package-framework keyword):
  `import framework "AppKit";` and `import framework { "AppKit", "Foundation" };`
- First milestone scope: S1 (link plumbing) + S2 (--init tbd harvest so linking
  stays self-contained, no Xcode/CLT), preserving the libSystem.tbd precedent.

## Known gaps this closes (from the old internal/issue note)

- EmitExecutableMachO (LLVMBackend.h ~5530-5543) has no -framework/-F handling.
- The harvested ~/.cflat/macsdk syslibroot contains only usr/lib/libSystem.tbd.
- (S3, later) BuildClangDriverArgs (LLVMBackend.h ~3882) has no macOS triple case -
  header imports parse with a Linux triple, no __APPLE__, no -isysroot.

## S1 - link plumbing

- Grammar (CFlat.g4): new importDeclaration alternative
  `Import 'framework' (StringLiteral | '{' StringLiteral (',' StringLiteral)* '}') ';'`
  with 'framework' as a soft keyword (text-matched like 'package'), not a lexer token.
- Dispatch at all FOUR import sites (ProcessImports, nested imports, Analyze/LSP,
  if-const branch in MainListener.h) + ForwardRefScanner no-op if needed.
- Backend: `cFrameworks_` accumulator (dedup, preserve order) beside cLinkLibs_;
  CLI symmetry: repeatable `--framework <name>` seeds it (mirrors --c-lib).
- EmitExecutableMachO: after the cLinkLibs_ loop, append `-framework <name>` per
  entry on BOTH link paths (bundled ld64.lld and the host-clang fallback). AppKit
  and Foundation imply nothing extra; do NOT auto-add -lobjc (programs use
  objc_getClass etc. via the framework reexports or declare -framework as needed).
- Targeting a non-macOS platform with `import framework` is a LogError (clean
  diagnostic, not a silent ignore). LSP mode: silently record, never error-paint.

## S2 - --init harvest extension (self-contained, no SDK)

- Extend the Darwin --init harvest (HarvestMacSystemStub, LLVMBackend.cpp ~3214):
  dlopen each curated framework from the dyld shared cache, walk its export trie
  (reuse MachoHarvestImage), and write a tbd v4 stub with the framework's real
  install-name at
  `~/.cflat/macsdk/System/Library/Frameworks/<Name>.framework/<Name>.tbd`.
  Curated v1 list: AppKit, Foundation, CoreFoundation. Also harvest
  /usr/lib/libobjc.A.dylib -> usr/lib/libobjc.tbd.
- ld64.lld with `-syslibroot <macsdk>` finds framework stubs under the standard
  System/Library/Frameworks path inside the root; verify, else pass an explicit -F.
- Link-time fallback order stays: harvested macsdk -> $SDKROOT -> xcrun SDK.
- Stretch (only if trivial): on-demand harvest at link time when a requested
  framework tbd is missing from the harvested root and no SDK exists.

## S3 (later) - Darwin header binding for C frameworks

macOS branch in BuildClangDriverArgs: `--target=arm64-apple-macosx11.0.0`,
`-isysroot` (SDKROOT/xcrun - headers require a real SDK; harvested stubs have no
headers), `-F` dirs; then `import package "CoreGraphics/CoreGraphics.h" framework
"CoreGraphics";` binds with auto-extern. AppKit headers are Objective-C and stay
OUT of scope (link-only + hand externs; the cocoa.cb bridge remains the ObjC path).

## S4 (DONE) - consumer

Switched core/cocoa.cb from dlopen(AppKit binary) to `import framework { "AppKit",
"Foundation" };`; kept dlsym but on RTLD_DEFAULT (extern-address casting of a C function
is unsupported by the compiler, so fn ptrs must still come from dlsym - documented above).
All example/macos + example/ui consumers go through cocoa.cb, so they inherited the change
with no source edits (only stale dlopen-mention comments updated in cocoa_window.cb and
ui_native_cocoa.cb). No compiler changes were needed. Docs updated (CLI.md, LANGUAGE.md).

## Acceptance for S1+S2 (on this arm64 box)

- New example `example/macos/framework_link.cb`: `import framework "CoreFoundation";`
  + hand extern `double CFAbsoluteTimeGetCurrent();`, plus
  `import framework { "AppKit", "Foundation" };` link presence; compiles, links via
  the bundled ld64.lld against the HARVESTED macsdk (no $SDKROOT), runs, exit 0.
- `cflat --init` regenerates macsdk with the new framework stubs, exit 0.
- `./test.sh Release` green; `./example_mac.sh` green (new example included).
- No behavior change on Windows/Linux paths (harvest code stays under __APPLE__;
  grammar/dispatch changes are platform-neutral; non-mac target -> LogError).
