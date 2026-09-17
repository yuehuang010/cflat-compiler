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

## Root cause (hypothesis)

The harvested libSystem / framework tbd stubs used by the self-contained link
(`internal/macos-build.md`) do not cover CoreGraphics, or the `framework` link clause for
these two examples does not add it. Every other Cocoa example (`ui_app`, `ui_counter`,
`sysinfo_mac`, `hello_objc`) links, so the gap is specific to CG symbols.

## Fix direction

Establish whether `CoreGraphics` is in the stub set; if not, harvest it the way the existing
frameworks are harvested, or make the two examples declare the framework explicitly. Acceptance:
`bash test_example.sh` reports 0 failed on macOS.
