# Plan: first-class macOS GUI (Cocoa) support

Goal: a Win32-controls-class GUI story on macOS. On Windows that is
`import "windows.h" lib {...}` + the example/ui host. The macOS equivalent is
AppKit, which is Objective-C - but the Obj-C runtime is a plain C ABI
(objc_msgSend / objc_getClass / sel_registerName), so cflat can drive all of
Cocoa without the compiler learning Objective-C.

## Current state (2026-07-01)

Working, SDK-free, via `example/macos/`:

- `cocoa.cb` - bridge library: dlopen of the AppKit framework binary + dlsym +
  typed `function<...>` casts of objc_msgSend (mandatory on arm64 anyway).
  Message helpers (msg0..msg3, msgRect for the NSRect-as-4-doubles HFA trick),
  runtime class creation for callbacks (objc_allocateClassPair +
  class_addMethod with a cflat function as IMP), and a small controls layer
  (cocoaApp/createWindow/createButton/createLabel/createTextField/runApp).
- `cocoa_window.cb` - window/label/textfield/button demo with cflat click
  handlers; has a --selftest mode.
- `hello_objc.cb`, `sysinfo_mac.cb` - console demos (Foundation objects,
  sysctl/libproc C APIs).

Fixed along the way: int-to-pointer casts now sign-extend signed sources
(`(void*)-2` == RTLD_DEFAULT; regression test testIntToPtrCast in
Test/test_c.cb).

Known gaps (see `internal/issue/p2/macos-header-import-and-framework-link.md`):

1. `BuildClangDriverArgs` (LLVMBackend.h ~3650) hard-codes
   `x86_64-pc-linux-gnu` on non-Windows, so `__APPLE__` is never defined and
   Apple header imports collapse (objc/runtime.h registers 1 of ~80 sigs).
2. `EmitExecutableMachO` (LLVMBackend.h ~5296) has no `-framework`/`-F`, and
   the harvested `~/.cflat/macsdk` syslibroot contains only libSystem.tbd -
   no framework stubs, no libobjc.

## Stage 1: framework linking (small, highest leverage)

Outcome: `import framework "AppKit";` (and `--c-framework AppKit`) lets a .cb
declare plain externs for objc_msgSend/objc_getClass/etc. and link them at
build time. Kills all the dlopen/dlsym/function-pointer-caching boilerplate
in cocoa.cb; symbol errors surface at link instead of null pointers at run.

Work items:

- Grammar/CLI: add a `framework "Name"` clause to the import-package rule
  (or a standalone `import framework "Name";` form) and a `--c-framework`
  flag in ArgParser.h. Route into a `cFrameworks_` list next to `cLinkLibs_`.
- `EmitExecutableMachO`: append `-framework <Name>` per entry to the ld64.lld
  invocation. Add `-F <syslibroot>/System/Library/Frameworks` when present.
- Stub availability, in order of preference:
  a. Extend `--init`'s dyld-shared-cache harvest (the export-trie walk that
     builds libSystem.tbd) to also emit
     `~/.cflat/macsdk/System/Library/Frameworks/<Name>.framework/<Name>.tbd`
     for a curated set (AppKit, Foundation, CoreFoundation, CoreGraphics)
     plus `usr/lib/libobjc.tbd`. Note: objc_msgSend lives in libobjc, which
     libSystem does NOT reexport - the dlopen bridge only dodged this because
     AppKit pulls libobjc transitively. Direct externs need `-lobjc` or the
     AppKit framework on the link line.
  b. Fallback: real SDK via $SDKROOT/xcrun (framework tbds already there),
     mirroring the existing libSystem fallback chain.
- On-demand harvest option: if `import framework "X"` names a framework not
  in the harvested set, harvest it lazily at compile time from the shared
  cache rather than failing (frameworks are cheap to walk).

Validation: rewrite cocoa.cb to plain externs + `import framework "AppKit";`
(keep the dlopen variant as a fallback path or delete it), all three macos
examples still run, `bash test.sh Release` green, and since linker/driver code
is shared, `test.bat` (Release) on the Windows box.

## Stage 2: Darwin triple for header binding (C frameworks)

Outcome: `import package "objc/runtime.h";`, sysctl.h, CoreFoundation and
CoreGraphics headers bind via the existing auto-extern machinery.

Work items:

- `BuildClangDriverArgs`: when targeting macOS, use the versioned triple
  already used for codegen (`arm64-apple-macosx11.0.0`) instead of the Linux
  triple, and pass `-isysroot` resolved the same way as
  `PosixSystemIncludeDirs` ($SDKROOT -> xcrun). Requires an SDK/CLT for the
  headers themselves - header import is inherently not SDK-free; document
  that (`--init`'s harvested stub covers linking only, not header text).
- Verify the extractor handles Apple-isms that now become visible:
  availability attributes, __attribute__((objc_...)) spillover in nominally-C
  headers, blocks (`^` types - should degrade to "skipped", not error).
- Re-test the existing Linux/WSL header-bind path afterward (same function,
  keep the Linux triple branch intact).

Validation: the repro in the issue file (`objc_getClass` after
`import package "objc/runtime.h"`) compiles and runs; `-v` shows ~80 sigs,
not 1. test.sh + test.bat both green.

## Stage 3: core cocoa library with ownership

Outcome: users write `import "cocoa.cb";` and get typed Window/Button/Label/
TextField with cflat lifetime semantics - the Win32-controls-equivalent
surface. This is a library, not a compiler feature (mirrors how example/ui
wraps Win32 rather than importing every header).

Work items:

- Promote the bridge from example/macos/cocoa.cb into `core/cocoa.cb` (or
  keep as a blessed example lib until API settles - decide then). Gate the
  file with `if const (__MACOS__)` like os.posix.cb.
- Map retain/release onto cflat ownership: wrapper struct holding the objc
  id, destructor sends `release`, copy/assign sends `retain` (or is move-
  only). Follow the single Cocoa convention: alloc/new/copy => owned (+1),
  everything else borrowed. This gives users ARC-like behavior for free.
- Autorelease pool management: push in cocoaApp(), and around any helper
  that runs before [NSApp run] takes over per-event pool draining.
- Keep the runtime-class machinery (makeTarget) but wrap it: a Button takes
  a cflat function/lambda-thin callback directly.
- Stretch: a Cocoa backend for the example/ui widget tree next to the GDI
  one, so the same app.cb runs on both OSes.

## Stage 4 (deferred, likely never): Obj-C header binding

Parsing AppKit.h itself needs `-x objective-c`, framework include resolution,
and a design for what an @interface becomes in cflat. The curated library in
Stage 3 makes this unnecessary for the GUI goal. Revisit only if users need
arbitrary Obj-C APIs beyond what the bridge pattern covers. New Apple APIs
are increasingly Swift-only (no C surface at all), so full coverage is
unreachable regardless; the Obj-C tier itself is stable/frozen.

## Ordering and effort

1 -> 2 -> 3; stage 4 parked. Stage 1 is the highest-leverage/smallest change.
Stages 1 and 2 touch shared compiler code (link driver, header extraction):
per project rule, each needs BOTH `bash test.sh Release` on the Mac AND
`test.bat` (Release) on Windows before being called done.

## Facts worth keeping in mind

- arm64 requires the typed-function-pointer cast of objc_msgSend even in C;
  cflat loses nothing vs clang here. No objc_msgSend_stret on arm64.
- NSRect (4 doubles) is an HFA in d0-d3: register-identical to 4 separate
  double args, so initWithContentRect: needs no by-value struct support.
- NSWindow defaults to releasedWhenClosed=YES - wrappers must set NO or the
  close button dangles our pointer.
- Cocoa coordinates are bottom-left origin, y grows upward (opposite GDI).
- Obj-C memory model is refcounting (retain/release); ARC is a clang
  compile-time feature, absent at the msgSend level - manual rules apply,
  which Stage 3's destructor mapping hides.
