# macOS binaries carry a DIFFERENT `LC_BUILD_VERSION` sdk depending on whether the cache is warm

**Filed 2026-08-05.** Investigation of the standing claim that "the `--init` bitcode cache changes
codegen" (leak counts drifting cold vs warm on `Test/test_move.cb`).

**The claim is FALSE and is refuted below.** The bitcode cache does not change program semantics.
What actually differs is the LINK: the presence of the harvested `macsdk` stub - which `--init`
writes into the same cache directory - flips the SDK version stamped into every macOS executable,
and macOS applies SDK-version-gated behaviours at runtime. This is a real defect, but a build-metadata
one, not a codegen one.

## Root cause

`cflat/LLVMBackend.h:8729-8743` (`EmitExecutableMachO`):

```cpp
const std::string stubRoot = MacStubSyslibroot();
std::string sdk = stubRoot;
std::string sdkVer = "11.0";                       // <- hardcoded
if (sdk.empty())
{
    if (const char* env = std::getenv("SDKROOT")) sdk = env;
    if (sdk.empty()) sdk = CaptureToolLine("xcrun --show-sdk-path 2>/dev/null");
    std::string v = CaptureToolLine("xcrun --show-sdk-version 2>/dev/null");
    if (!v.empty()) sdkVer = v;                    // <- only on the FALLBACK path
}
... "-platform_version", "macos", "11.0.0", sdkVer, "-syslibroot", sdk ...
```

`sdkVer` is only ever set to the real SDK version on the branch where the harvested stub is
ABSENT. `MacStubSyslibroot()` (`cflat/LLVMBackend.cpp:3799-3806`) returns the stub root iff
`<cache dir>/macsdk/usr/lib/libSystem.tbd` exists, and that file is written by `--init` /
`--init-local`. So the cache directory - a pure-performance artifact by contract - decides the
SDK version of every emitted executable:

| cache state | `-platform_version` | resulting `LC_BUILD_VERSION` |
|---|---|---|
| warm (`--init` run; stub present) | `macos 11.0.0 11.0` | `minos 11.0`, **`sdk 11.0`** |
| cold (no cache / cleared) | `macos 11.0.0 26.5` | `minos 11.0`, **`sdk 26.5`** |

The `11.0` is also a FALSE declaration on its own terms. `HarvestMacSystemStub`
(`cflat/LLVMBackend.cpp:3809`) builds the stub by walking the export tries of the **live dyld
shared cache** (`/usr/lib/system/*` of the running OS - 26.5 here), not from a macOS 11.0 SDK.
The binary is linked against 26.5 symbols while claiming it was built against the 11.0 SDK. The
deployment target (`minos 11.0.0`) is the separate, correct knob and should stay 11.0.

## Repro

```bash
cd <repo>
mkdir -p scratch/cachediag/bare
cp -R x64/Release/.cflat scratch/cachediag/cold && rm -rf scratch/cachediag/cold/runtime

# A: warm - core bitcode HIT, stub present
x64/Release/cflat Test/test_move.cb -i Test/library -o scratch/cachediag/warm.exe \
  --out-lli scratch/cachediag/warm.ll -v
# B: cold bitcode, stub still present (isolates the bitcode cache alone)
CFLAT_CACHE_DIR=$PWD/scratch/cachediag/cold x64/Release/cflat Test/test_move.cb -i Test/library \
  -o scratch/cachediag/cold.exe --out-lli scratch/cachediag/cold.ll -v
# C: bare - no bitcode, NO stub (what "delete ~/.cflat" gives you)
CFLAT_CACHE_DIR=$PWD/scratch/cachediag/bare x64/Release/cflat Test/test_move.cb -i Test/library \
  -o scratch/cachediag/bare.exe --out-lli scratch/cachediag/bare.ll -v

for x in warm cold bare; do leaks --atExit -- ./scratch/cachediag/$x.exe | grep "leaks for"; done
```

Measured on `7a03e9f`, macOS 26.5 arm64, Release, three runs each, fully stable:

| binary | bitcode cache | stub | `-platform_version` sdk | `leaks --atExit` |
|---|---|---|---|---|
| A warm | hit | yes | 11.0 | **14 leaks / 272 bytes** |
| B cold | miss | yes | 11.0 | **14 leaks / 272 bytes** |
| C bare | miss | no | 26.5 | **16 leaks / 320 bytes** |

A vs B is the bitcode cache alone: **identical**. B vs C is the stub alone: **the whole delta**.

The prior reports of 13 vs 15 leaks (256 vs 304 bytes) are this same defect on a different tree -
note the delta is byte-for-byte the same, **2 leaks / 48 bytes**, in both measurements.

## Proof it is the SDK stamp and nothing else

`B` (cold) and `C` (bare) were compiled from **byte-identical IR**:

```bash
diff scratch/cachediag/cold.ll scratch/cachediag/bare.ll   # no output
```

and their Mach-O `__TEXT,__text` sections are byte-identical
(`diff <(otool -s __TEXT __text cold.exe) <(otool -s __TEXT __text bare.exe)` is empty). The only
load-command differences are `uuid`, `sdk 11.0` vs `sdk 26.5`, and libSystem's `current version`
(1.0.0 from the stub vs 1356.0.0 from the SDK tbd).

Decisive test - patch **only** the 4-byte `sdk` field of `LC_BUILD_VERSION` in `cold.exe`
(0x000B0000 -> 0x001A0500), re-sign, re-measure:

```bash
python3 -c "
import struct,shutil
shutil.copy('cold.exe','patched.exe')
d=bytearray(open('patched.exe','rb').read())
off=d.find(struct.pack('<II',0x32,32))          # LC_BUILD_VERSION, cmdsize 32
struct.pack_into('<I',d,off+16,0x001A0500)      # sdk 11.0 -> 26.5
open('patched.exe','wb').write(d)"
codesign -f -s - patched.exe
leaks --atExit -- ./patched.exe | grep "leaks for"
```

`cold.exe` -> 14 leaks / 272 bytes. `patched.exe` -> **16 leaks / 320 bytes**, three runs, stable.
One flipped metadata field, same machine code, different runtime behaviour.

## The bitcode cache is exonerated (10-test sweep)

Warm vs cold-with-stub, `Test/*.cb`, both under the same `sdk 11.0` stamp:

| test | `leaks --atExit` warm vs cold |
|---|---|
| test_move | 14/272 = 14/272 |
| test_interface | 3/64 = 3/64 |
| test_collection_leaks | 0/0 = 0/0 |
| test_list_ownership | 0/0 = 0/0 |
| test_core | 0/0 = 0/0 |
| test_operators | 0/0 = 0/0 |
| test_module | 0/0 = 0/0 |
| test_stream | 9/18544 = 9/18544 |
| test_generics, test_allocators | no leaks output (no exe produced under `-o` sweep) |

Function-level IR comparison of `test_move.cb` (404 defines on both sides): identical `define` /
`declare` / named-global sets; after numeric-suffix normalization only 10 bodies differ, and every
one of those is LLVM value-name uniquing (`%.unpack4` vs `%.unpack8`) with identical operands -
the warm module inherits names from the deserialized bitcode. **No missing or extra destructor
call, no malloc/free asymmetry, nothing an ownership analysis reads.** The `--init` serializer
round-trip is not implicated.

## Secondary, cache-keyed but benign (do not confuse with the above)

Two further differences between warm and cold IR, both non-semantic, both worth knowing because
they defeat naive `diff`-based "the cache is pure" checks:

- The warm module keeps `target triple = "arm64-apple-macosx"` (carried in the bitcode); the cold
  module has no `target triple` line. Codegen uses an explicit `TargetMachine` either way.
- Global emission order differs (the deserialized module's globals come first, so `@__FILE__` and
  the string pool land at different indices). This shifts constant addresses, so warm and cold
  `__text` are NOT bit-identical even though they are semantically equal. cflat is therefore not
  bit-reproducible across cache states; that is a separate, lower-value observation.

## Direction of wrongness

**The warm/cached side is the wrong one.** `sdk 11.0` is a claim the binary cannot support: it
was linked against symbols harvested from the running 26.5 system. macOS gates real behaviour on
this field - `test_stream` allocates 2048 fewer libSystem bytes under the 11.0 stamp - so every
binary built with a populated cache (which is *every* binary from `test.sh`, `example_mac.sh`, and
any developer who ran `--init`) is silently opted into legacy compatibility behaviour. The cold
path already does the right thing.

## Fix direction

Hoist the SDK-version query out of the fallback branch: resolve `sdkVer` from
`xcrun --show-sdk-version` (or, better, record the OS version at harvest time into the `macsdk`
tree so the stub carries its own provenance and no `xcrun` is needed - the self-contained-build
property must not regress) and use it on BOTH branches. `11.0` stays as the last-resort default
only when nothing else is knowable. Leave `minos 11.0.0` alone. While there, consider stamping the
harvested libSystem stub's `current-version` so the `LC_LOAD_DYLIB` version stops reading 1.0.0.

## Blast radius

Every macOS executable cflat emits - the field is unconditional on the `-o` path. Nothing on
Windows or Linux. No test currently asserts on it, and the full suite is green under both stamps,
which is why it survived. The concrete cost so far is measurement: **leak counts are not comparable
across cache states**, and that produced a multi-agent false alarm that the bitcode cache changes
codegen. Any future ownership/leak measurement must fix the cache state (or the stamp) across the
binaries it compares.

## Bucket note

Filed P2 as silent wrong build metadata affecting every emitted binary, with a measurable runtime
consequence and a documented cost to the ownership campaign's tooling. P3 is defensible - no
program is miscompiled and no test fails - and the maintainer should re-rank if the "latent, no
witness of a wrong program" reading wins.
