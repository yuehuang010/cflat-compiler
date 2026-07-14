# --asan: Windows/COFF and macOS/Mach-O integration fixes

The in-process LLVM AddressSanitizer (`--asan`, `AddressSanitizerPass` in `LLVMBackend.cpp::OptimizeModule`, gated on `asan_`) had TWO Windows/COFF defects that made it unreliable. Both FIXED 2026-06-08. macOS/Mach-O support landed 2026-07-13 (see below).

## Bug 1: static shadow-offset mismatch -> every check is a raw access-violation
We link the DYNAMIC asan runtime (`clang_rt.asan_dynamic-x86_64`), which maps its shadow at a base chosen at runtime and published in `__asan_shadow_memory_dynamic_address`. But the pass BAKES IN a static Windows shadow offset (`kWindowsShadowOffset64 = 0x100000000000`), so the instrumented shadow load targets `(addr>>3)+0x100000000000` - NOT where the dynamic runtime placed shadow. The shadow-byte READ itself faults => every check surfaces as `AddressSanitizer: access-violation on unknown address <shadowaddr>` instead of a clean report, and freed memory isn't seen as poisoned (so UAFs read real unmapped memory). It was ASLR/layout-sensitive => "worked" intermittently, masking the bug.
DIAGNOSIS TRICK: in the register dump, `(rax>>3) + rbx == rdi(faulting addr)` where rbx was 0x100000000000 = the static offset. That arithmetic match is the smoking gun for a shadow-offset mismatch.
FIX: force the dynamic shadow so the pass emits a load of `__asan_shadow_memory_dynamic_address` (what clang-cl does on Windows). Set the LLVM cl::opt before running the pass:
```cpp
auto& reg = llvm::cl::getRegisteredOptions();
auto it = reg.find("asan-force-dynamic-shadow");
if (it != reg.end()) static_cast<llvm::cl::opt<bool>*>(it->second)->setValue(true);
```
(needs `#include <llvm/Support/CommandLine.h>`).

## Bug 2: COFF object-writer abort on private globals
`LLVM fatal error: Associative COMDAT symbol '__string_empty.<hash>' does not exist.` On COFF the default global-GC path (`InstrumentGlobalsCOFF`) emits each instrumented global's metadata in a COMDAT associative to the instrumented global's symbol; cflat's PRIVATE internal globals (e.g. `__string_empty`, LLVMBackend.h ~1690) don't survive that path. It's content-sensitive (a core-lib edit shifting global layout flips it on/off).
FIX (do NOT use `UseGlobalGC=false` - that BREAKS the dynamic shadow, reintroducing Bug 1!): mark every global `NoAddress` so ASan skips global instrumentation (no redzones, no associative COMDATs), keeping UseGlobalGC at default:
```cpp
for (llvm::GlobalVariable& G : module->globals()) {
    auto md = G.hasSanitizerMetadata() ? G.getSanitizerMetadata() : llvm::GlobalValue::SanitizerMetadata();
    md.NoAddress = true; G.setSanitizerMetadata(md);
}
```
Cost: no global-buffer-overflow detection. Heap/stack/function (load/store) instrumentation - what we need for use-after-free - is unaffected.

## Verified
`out/uaf2.cb` (aliased ptr so cflat's delete-null doesn't mask it) -> clean `heap-use-after-free` with shadow map; a clean program exits 0 with no report. test.bat green (all asan code gated on `asan_`). ASan runtime loads (`ASAN_OPTIONS=help=1` prints flags). One-liner to build a test under asan: `x64/Debug/cflat.exe <in.cb> -i <imp> --asan -g -o <out.exe>`.

## Caveat for exit-phase / corrupting crashes
asan can still emit a FRAMELESS report (header only, dies mid-report) when the crash corrupts the heap or happens during teardown - the symbolizer/stack-walk itself faults, even with symbolize=0 + fast_unwind. asan is reliable for in-bounds-lifetime UAFs (stored alloc/free stacks) but a frameless report on a teardown crash means you fall back to cdb.

# macOS / Mach-O (arm64) - FIXED 2026-07-13

`--asan` died at object emission with `LLVM fatal error: MachO doesn't support COMDATs, 'asan.module_ctor' cannot be lowered.` Three fixes, all gated on `targetMacOS_` so the Windows path is unchanged.

## Bug 3: the pass ran with an EMPTY module triple (root cause of the COMDAT abort)
`Init()` sets only the module's DATA LAYOUT, never its triple; the Mach-O triple is set later in `EmitExecutableMachO`, AFTER `OptimizeModule` has run. LLVM reads the triple from the MODULE, and `Triple("")` defaults to ELF - so the pass took its ELF branch and stamped a COMDAT on `asan.module_ctor` (that `setComdat` is gated on `isOSBinFormatELF()`). Mach-O emission then aborts, since Mach-O has no COMDATs.
FIX: set the module triple to the versioned `arm64-apple-macosx11.0.0` (matching `EmitExecutableMachO`) BEFORE running the pass.
NOTE: this same empty triple is why Bug 1 above looked like a "static Windows offset" - an empty triple falls back to `kDefaultShadowOffset64 = 1<<44 = 0x100000000000`. In real LLVM 18 `kWindowsShadowOffset64` is ALREADY the dynamic sentinel, so the `asan-force-dynamic-shadow` force has been compensating for the empty triple all along. Windows never hit the COMDAT abort only because COFF DOES support COMDATs. Left as-is (it works); worth revisiting if the Windows asan path is ever touched again.

## Bug 4: Apple runtime exports no v8 version-check symbol
LLVM 18 emits a call to `___asan_version_mismatch_check_v8`; Apple's clang-21 runtime only exports `___asan_version_mismatch_check_apple_clang_2100` -> link fails on the undefined symbol.
FIX: `asanOpts.InsertVersionCheck = false;` on macOS. Safe: the rest of the v8 ABI (`___asan_init`, the load/store checks, `___asan_report_*`, the globals register/unregister hooks) IS exported by Apple's runtime (checked with `nm`), and a real UAF reports correctly.

## Bug 5: Apple runtime's interceptor self-check false-positives on cflat's `puts`
Apple's asan runtime runs `VerifyInterceptorsWorking` at startup: it `dlsym(RTLD_DEFAULT, "puts")` and expects the result to land inside the asan dylib. cflat DEFINES its own `puts` (`core/cruntime.cb`), and main-executable symbols win that lookup - so the check always fails and hard-aborts every cflat asan binary before `main` with "Interceptors are not working".
FIX: emit a strong `__asan_default_options()` returning `"verify_interceptors=0"` (the documented override hook; a strong definition beats the runtime's weak default). This suppresses ONLY that self-check - malloc/free/operator-new/delete interception is genuinely active, proven by the UAF report's `malloc`/`free` frames resolving into `libclang_rt.asan_osx_dynamic.dylib`.

## Link line (Mach-O)
`EmitExecutableMachO` appends `<clang-resource-dir>/lib/darwin/libclang_rt.asan_osx_dynamic.dylib` plus `-rpath <that dir>` (the dylib's install name is `@rpath/...`). The resource dir comes from `clang -print-resource-dir`, hoisted so it is only shelled out once (it also sources `libclang_rt.osx.a`). `--asan` on macOS therefore REQUIRES Xcode or the Command Line Tools - the SDK-free harvested-stub link cannot supply the runtime, and `LogError`s if it is absent. The clang-driver fallback path passes `-fsanitize=address`.

## Verified (macOS arm64)
`scratch/asan_uaf.cb` (aliased ptr) -> clean `heap-use-after-free` with alloc + free stacks and shadow map, no env vars needed. `Test/test_math.cb --asan` runs clean, exits 0, no false positives. `test.sh Release` green (165 passed, 0 failed). Build a test under asan: `x64/Release/cflat <in.cb> -i <imp> --asan -g -o <out>`.

## Shared caveat: custom allocators are invisible to asan
cflat objects allocated through the pooled/arena allocators (`core/arena.cb`, `core/block_allocator.cb`) do not hit `malloc` per object, so a UAF INSIDE those pools is invisible to asan (and to guard-malloc). Only the default `malloc` path (`core/memory.cb`) is covered. Same on Windows.
