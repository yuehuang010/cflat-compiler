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
- `ui/ui-native-canvas-input-images-win32-winui` - GDI canvas host image/input parity, mostly
  closed, residual gaps.

(Closed 2026-08-14: winrt-self-new-missing-vtable FIXED; winui-icontrol-get-template-misreads
measured benign; winmd-scrollviewer-statics-vtable-mismatch measured NOT a projection bug -
slots match the SDK ABI, the crash was an IUIElement-for-IDependencyObject argument mismatch,
caller contract now noted at the winuiZeroItemsCache site in winui.cb.)

Portable:
- `p2/file-offsets-capped-at-2gb` - `filesystem.cb` narrows offsets through `int` / C `long`,
  capping file I/O at 2GB on every platform.

## Fix direction and sequencing

- `p2/file-offsets-capped-at-2gb` is portable, verifiable on any host, and independent of
  everything else here. Do it first and separately.
- `p2/macos-header-import-and-framework-link` is verifiable on this host and unblocks the most
  downstream work. Two parts: correct the Darwin triple, and emit `-framework`/`-F`. See
  `internal/macos-build.md` for the existing link path.
- The WinMD slot measurements are complete. Keep attached-property arguments at their declared
  ABI interface type; do not infer IDependencyObject calls from an IUIElement pointer.
- Everything under Windows/WinRT needs a Windows host to verify. Do not land those from a macOS
  session on reasoning alone.
