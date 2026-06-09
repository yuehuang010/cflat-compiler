# --asan: Windows/COFF integration fixes

The in-process LLVM AddressSanitizer (`--asan`, `AddressSanitizerPass` in `LLVMBackend.cpp::OptimizeModule`, gated on `asan_`) had TWO Windows/COFF defects that made it unreliable. Both FIXED 2026-06-08.

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
