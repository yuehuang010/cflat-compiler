# The C++ type-request disk cache is not keyed on the bitfield packing mode

Found 2026-09-16 by the review of `fix/cpp-bitfields-itanium` (landed). Before that change
`PackBitfields` was target-independent, so a cached packed layout could never disagree with the
current target. Now imported records pack by the Itanium rule off Windows and the MSVC rule on
`--platform win64`, but `CxxTypeRequestCacheKey` (`cflat/LLVMBackend_CInterop.cpp` ~4035:
headers, includes, defines, mtime, clang args, build stamp) has no ABI-mode component.

## Repro (clean cache dir)

```
cflat scratch/win7.cb -i scratch --platform win64 --check   # refused (expected on this host)
cflat scratch/win7.cb -i scratch --check                    # ALSO refused: cflat 8 bytes, clang 4
```

A fresh copy of the same header compiles host-first PASS, and host-first-then-win64 leaves the
host result PASS. The wrong-direction reuse is caught by `VerifyCxxRecordLayout` as a refusal,
never as wrong code, and cross-targeting win64 from macOS is not a working configuration anyway,
so the impact is bounded.

## Fix direction

Add the packing mode (or the resolved target triple) to the type-request cache key, and to the
header disk cache key if the packed record layout is stored there. Acceptance: the two-line repro
above passes on the second line.
