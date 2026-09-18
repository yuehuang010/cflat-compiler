# test_example.sh: mempress_mac and framework_link fail at the SDK-free link on CoreGraphics symbols

## Summary

On macOS arm64 the example gate reports 43 passed, 2 failed at master df973fc2 (2026-09-16).
Both failures are link errors, not compile errors: `out/mempress_mac` and `out/framework_link`
reach `ld64.lld` (SDK-free) and stop on undefined CoreGraphics symbols
(`_CGBitmapContextCreate`, `_CGColorSpaceCreateDeviceRGB`, `_CGContextDrawImage`,
`_CGRectContainsPoint`, `_CGRectGetWidth`, ...). Seen identically by the 2026-09-16 dogfood
agent before today's commits, so it is pre-existing, not a regression of any of them.

## Repro

```
bash test_example.sh
grep 'undefined symbol' <log>
```

## Root cause (measured 2026-09-17)

The SDK's `CoreGraphics.tbd` declares a target the bundled `ld64.lld` does not know, so the
stub never loads and every `_CG*` symbol is then undefined:

```
ld64.lld: error: could not load TAPI file at
  .../MacOSX.sdk/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics.tbd:
  malformed file
  ...CoreGraphics.tbd:4:20: error: unknown target
                     arm64e.x1-macos, arm64e.x1-maccatalyst ]
```

`arm64e.x1` is a sub-architecture spelling recent Xcode emits; it is not in the TAPI target
table of the pinned LLVM's `ld64.lld`. Any CFlat program linking CoreGraphics on this host
fails the same way - only these two examples do. Environmental, not a compiler defect: neither
example imports C++, and the error comes out of the linker reading a SYSTEM file.

## Fix direction

Options, cheapest first:

- Harvest/patch the .tbd the way the self-contained libSystem stub is harvested
  (`internal/macos-build.md`): strip the unknown sub-architecture targets from the copy cflat
  feeds the linker.
- Teach the framework-link path to fall back to the `.framework` binary / a real `-framework`
  search when a .tbd fails to parse.
- Bump the pinned LLVM to a version whose TAPI reader accepts `arm64e.x1` (check before
  assuming - the pinned version is deliberate and lives in `CMakePresets.json`).

Acceptance: `bash test_example.sh` reports 0 failed on macOS. Until then the mac example gate
cannot reach 0 failures on a host with this Xcode; record the count as
`43 passed, 2 failed (both this issue)` rather than treating it as a regression.
