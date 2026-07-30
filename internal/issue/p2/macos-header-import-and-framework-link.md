# macOS: header import parses with a Linux triple; no -framework link support

## Summary

Two gaps block first-class Apple-API binding on macOS (the example/macos
demos work around both via dlopen/dlsym + typed objc_msgSend casts):

1. **Header import uses the wrong target on Darwin.** `BuildClangDriverArgs`
   (LLVMBackend.h ~3650) hard-codes `x86_64-pc-linux-gnu` for all non-Windows
   targets, so the in-process clang parse of an imported header never defines
   `__APPLE__`/`__MACH__` and passes no `-isysroot`. Plain portable headers
   (math.h) bind fine, but Apple headers gated on `__APPLE__` misparse:
   `import package "objc/runtime.h";` registers only 1 of ~80 functions
   (verified with -v: "header bind: 1 sig(s)"). Framework headers (AppKit.h)
   are Objective-C and would need `-x objective-c` plus a framework include
   path on top of the triple fix.

2. **No framework linking.** `EmitExecutableMachO` (LLVMBackend.h ~5296)
   emits no `-framework`/`-F`; the only extra-lib channel is `cLinkLibs_`
   (--c-lib / inline `lib` clause) passed verbatim to ld64.lld. Additionally
   the default `-syslibroot` is the harvested `~/.cflat/macsdk`, which
   contains only `usr/lib/libSystem.tbd` - no `System/Library/Frameworks` -
   so framework tbds are unresolvable unless the real SDK fallback is active.

## Repro

```cflat
import package "objc/runtime.h";
extern int main() { void* c = (void*)objc_getClass("NSObject"); return 0; }
// -> "Undefined variable objc_getClass." (function never registered)
```

## Fix direction

- (a) In `BuildClangDriverArgs`, pick `arm64-apple-macosx11.0.0` when
  `targetMacOS_` and pass `-isysroot` (SDKROOT / xcrun, mirroring
  `PosixSystemIncludeDirs`).
- (b) Add a `framework "Name"` import clause / `--c-framework` flag that
  appends `-framework Name` to the ld64.lld line, and harvest (or fall back
  to) framework tbds under the syslibroot.
- Until then, the supported pattern is the example/macos/cocoa.cb bridge:
  dlopen("/System/Library/Frameworks/X.framework/X") + dlsym + `function<...>`
  casts (which arm64 objc_msgSend requires anyway).
