# q18: Platform, C interop, and native UI

8 items. Platform-specific gaps. Grouped by host rather than by root cause, because each host's
items share a toolchain and a verification story, not a code path.

## Members

macOS:
- `p2/macos-header-import-and-framework-link` - the clang driver hardcodes a Linux triple on
  Darwin, and no `-framework`/`-F` linking is emitted. This is the blocker for header-imported
  system frameworks and therefore for the Cocoa GUI direction.
- `ui/ui-native-visual-polish-win32-winui` - font-variant styling exists only in Cocoa's
  `_applyNode`/`_syncProps` path; the Win32/WinUI side has no equivalent.

Windows / WinRT:
- `ui/win32-classic-common-controls-v5` - the emitted exe has no manifest at all, so it binds
  classic v5 comctl32 instead of themed v6.
- `ui/winrt-self-new-missing-vtable` - the `winrtClasses` entry is populated only after the
  class's own method bodies are codegen'd, so a self-`new` sees a not-yet-registered class.
- `ui/winmd-scrollviewer-statics-vtable-mismatch` - statics-interface slot index likely diverges
  from runtime vtable ordering (UNDIAGNOSED).
- `ui/winui-icontrol-get-template-misreads` - probe reading unconfirmed; may be the wrong slot
  offset or simply an unmeasured control (UNDIAGNOSED).
- `ui/ui-native-canvas-input-images-win32-winui` - GDI canvas host image/input parity, mostly
  closed, residual gaps.

Portable:
- `p2/file-offsets-capped-at-2gb` - `filesystem.cb` narrows offsets through `int` / C `long`,
  capping file I/O at 2GB on every platform.

## Fix direction and sequencing

- `p2/file-offsets-capped-at-2gb` is portable, verifiable on any host, and independent of
  everything else here. Do it first and separately.
- `p2/macos-header-import-and-framework-link` is verifiable on this host and unblocks the most
  downstream work. Two parts: correct the Darwin triple, and emit `-framework`/`-F`. See
  `internal/macos-build.md` for the existing link path.
- `ui/winrt-self-new-missing-vtable` is a registration-ordering fix (register the class before
  codegen'ing its bodies) and is the only WinRT item with a clear root cause - do it before the two
  undiagnosed vtable items, since a fix there may change what they measure.
- The two UNDIAGNOSED WinRT items need a measurement, not a fix: dump the actual runtime vtable
  ordering and compare against the projected slot index before proposing anything.
- Everything under Windows/WinRT needs a Windows host to verify. Do not land those from a macOS
  session on reasoning alone.
