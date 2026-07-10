#!/usr/bin/env python3
"""
Bulk LSP test: open every real .cb source file in the repo through the
LSP server and assert two invariants on the diagnostics it reports:

  1. No Error-severity diagnostics.
  2. Every "'<name>' is never used" hint points at a source token that
     actually equals <name>. A mismatch means the diagnostic was attributed
     to the wrong file/line - e.g. a generic template local (core/list.cb)
     leaking a faded hint onto the use-site file's unrelated line (an import).
     This is a self-validating correctness check: it needs no allowlist and
     produces no false positives on legitimate hints (whose range, by
     construction, spans exactly the named identifier).

Files swept:
  - Test/*.cb           (top-level test programs)
  - example/**/*.cb     (recursive)
  - cflat/core/*.cb     (standard library, excluding runtime.cb)

Usage:
    python vscode-extension/test/lsp_bulk_test.py [path/to/cflat.exe] [extra lsp args]

Exit code: 0 = every file clean, 1 = at least one file produced an error
diagnostic, a misattributed hint, or the server died / timed out.
"""
# Defer annotation evaluation so PEP 604 unions (str | None) run on Python 3.9.
from __future__ import annotations

import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lsp_client import LspClient, find_exe, initialize, wait_diagnostics_for

REPO_ROOT = Path(__file__).parent.parent.parent

# DiagnosticSeverity: 1=Error, 2=Warning, 3=Info, 4=Hint
ERROR_SEVERITY = 1
HINT_SEVERITY = 4

# Per-file diagnostics timeout (seconds). Some files (heavy generics) are slow.
DIAG_TIMEOUT = 300.0

# Matches both the local/param form ("'foo' is never used") and the function
# form ("function 'foo' is never used"); the captured group is the identifier
# the hint claims is unused, which must match the token under its range.
_NEVER_USED_RE = re.compile(r"'(\w+)' is never used$")


def find_misattributed_hints(text: str, diags: list) -> list[str]:
    """Return a description for each "never used" hint whose range does not span
    the identifier it names. Empty list means all such hints are well-placed."""
    lines = text.split("\n")
    problems: list[str] = []
    for d in diags:
        m = _NEVER_USED_RE.search(d.get("message", ""))
        if not m:
            continue
        name = m.group(1)
        rng = d.get("range", {})
        start, end = rng.get("start", {}), rng.get("end", {})
        ln = start.get("line", -1)
        c0 = start.get("character", 0)
        c1 = end.get("character", c0)
        if ln < 0 or ln >= len(lines):
            problems.append(
                f"L{ln+1}: claims '{name}' unused but that line is outside the "
                f"file ({len(lines)} lines) - misattributed from another file")
            continue
        actual = lines[ln][c0:c1]
        if actual != name:
            problems.append(
                f"L{ln+1}:{c0}: claims '{name}' is never used but the source "
                f"there is '{actual}' - misattributed diagnostic")
    return problems


# example/COM/direct2d_demo.cb imports "Windows.Win32.winmd", which ships only in the
# Microsoft.Windows.SDK.Win32Metadata NuGet package (not the system), so it resolves only
# when -i points at that package. The package is per-user and version-specific.
WIN32_METADATA_DEMO = "example/COM/direct2d_demo.cb"

# fedit imports its framework from a sibling directory (`import "win32_native_host.cb"`
# resolved against example/ui). The sweep passes `-i example/ui` alongside the
# Win32-metadata `-i` (multiple -i dirs are searched in order), so fedit analyzes clean here.
UI_FRAMEWORK_DIR = "example/ui"


def find_win32_metadata_dir() -> Path | None:
    """Return the newest cached Win32-metadata package dir containing Windows.Win32.winmd,
    or None if the NuGet package is not installed for this user."""
    root = Path(os.path.expanduser("~")) / ".nuget" / "packages" / "microsoft.windows.sdk.win32metadata"
    if not root.is_dir():
        return None

    def version_key(p: Path):
        # Leading numeric components of e.g. "70.0.11-preview" -> (70, 0, 11); newest wins.
        head = re.match(r"(\d+(?:\.\d+)*)", p.name)
        return tuple(int(n) for n in head.group(1).split(".")) if head else (0,)

    candidates = sorted(
        (d for d in root.iterdir() if d.is_dir() and (d / "Windows.Win32.winmd").is_file()),
        key=version_key,
    )
    return candidates[-1] if candidates else None


# OS-platform alternates: os.windows.cb and os.posix.cb declare the SAME libc/syscall
# functions (fread/fwrite/fopen/...) as bare-linkage externs, one set per platform.
# os.cb imports exactly one of them via `if const (__WINDOWS__)`, so only the host's
# file is ever in scope in real use. The bulk sweep opens each file standalone with the
# host runtime auto-imported, so analyzing the NON-host file collides its externs with
# the host's (e.g. os.posix.cb's `fread` vs os.windows.cb's `fread` on Windows). Skip
# the non-host alternate - it is exercised on its own platform's run.
NON_HOST_OS_FILE = "os.posix.cb" if os.name == "nt" else "os.windows.cb"

HOST_IS_WINDOWS = os.name == "nt"
HOST_IS_MACOS = sys.platform == "darwin"

# Windows/environment-only sources that cannot analyze clean on a non-Windows host:
# they import windows.h/winsock2.h/COM headers, use os.windows.* or WinRT (.winmd),
# or need a vcpkg package installed. The sweep runs the locally-built native cflat,
# so the host OS is the analysis target. Mirrors test.sh's SKIP-list rationale; the
# Windows CI run (test_lsp.bat) still sweeps every one of these. Skipped only off
# Windows - keep in sync with the same-named content in test.sh when files move.
_WIN_ONLY_DIRS = ("example/windows", "example/COM", "example/vcpkg")
_WIN_ONLY_FILES = {
    # Test/: os.windows.* usage, WinMD, or CRT header binding that collides with POSIX libc.
    "test_crt.cb", "test_stream.cb", "test_threadpool.cb",
    "test_windows.cb", "test_windows_cache.cb", "test_winmd.cb",
    # example/hpc: os.windows.* high-resolution timing.
    "heat_stencil.cb", "nbody2.cb", "spectrum.cb",
    # example/restAPI: the server half uses os.windows.* (winsock); the client half is portable.
    "benchmark.cb", "http_server.cb", "rest_server.cb",
    "test_http_server.cb", "test_rest_features.cb", "test_rest_server.cb", "todo_api.cb",
    # example/shell: os.windows.* console / _WIN32_FIND_DATAA.
    "dir.cb", "fsh.cb", "stars.cb",
    # example/ui: the win32_* backends and the os.windows.* framework demos.
    # (tui_demo.cb, formerly host.cb, no longer uses os.windows.* directly - the
    # host loop moved to core/ui_canvas/term.cb - so it is portable and not skipped.)
    "boxes.cb", "win32_boxes.cb",
    "win32_native_settings.cb", "win32_settings.cb", "win32_shot.cb",
    # core: WinRT projection library (the two Win32 UI hosts are path-qualified below,
    # since core/ui_native/win32.cb and core/ui_canvas/win32.cb share the basename win32.cb).
    "winrt.cb",
}

# Windows-only sources whose basename alone is ambiguous (core/ui_native/win32.cb and
# core/ui_canvas/win32.cb both import windows.h and are both win32.cb by basename), so
# these are matched by full repo-relative path instead of going in _WIN_ONLY_FILES.
_WIN_ONLY_PATHS = {
    "cflat/core/ui_native/win32.cb",
    "cflat/core/ui_canvas/win32.cb",
}


def is_windows_only(path: Path) -> bool:
    rel = path.relative_to(REPO_ROOT).as_posix()
    if any(rel == d or rel.startswith(d + "/") for d in _WIN_ONLY_DIRS):
        return True
    if rel in _WIN_ONLY_PATHS:
        return True
    return path.name in _WIN_ONLY_FILES


# macOS-only sources: `import framework` is rejected unless the target is macOS, and the
# Darwin SDK headers it binds (CoreGraphics/...) do not exist on other hosts. The rest of
# example/macos/ analyzes clean anywhere (it dlopens AppKit at runtime, binds no SDK
# header), so skip by name rather than by directory. Skipped only off macOS.
_MAC_ONLY_FILES = {"framework_link.cb"}


def is_macos_only(path: Path) -> bool:
    return path.name in _MAC_ONLY_FILES


def collect_files(include_win32_demo: bool) -> list[Path]:
    files: list[Path] = []
    files += sorted((REPO_ROOT / "Test").glob("*.cb"))
    files += sorted((REPO_ROOT / "example").rglob("*.cb"))
    files += sorted(
        p for p in (REPO_ROOT / "cflat" / "core").glob("*.cb")
        if p.name != "runtime.cb" and p.name != NON_HOST_OS_FILE
    )
    # The NativeHost/Canvas host backends live in two subdirectories (core/ui_native/,
    # core/ui_canvas/) rather than flat in core/, so the non-recursive glob above misses
    # them; sweep those two directories explicitly to keep the same coverage as before
    # they were split out of core/*.cb.
    files += sorted((REPO_ROOT / "cflat" / "core" / "ui_native").glob("*.cb"))
    files += sorted((REPO_ROOT / "cflat" / "core" / "ui_canvas").glob("*.cb"))
    if not include_win32_demo:
        demo = (REPO_ROOT / WIN32_METADATA_DEMO).resolve()
        files = [f for f in files if f.resolve() != demo]
    # example/ui/winui/* import the WinUI 3 runtime winmds (Microsoft.UI.Xaml.winmd) from a
    # sibling `winmd/` dir plus the Windows App SDK bootstrapper; the bare sweep has neither
    # the -i dir nor the App SDK, so it cannot resolve them. They are exercised by example.bat's
    # --worker-winui gate with the right flags. Skip them here (like the Win32-metadata demo).
    winui_dir = (REPO_ROOT / "example" / "ui" / "winui").resolve()
    files = [f for f in files if winui_dir not in f.resolve().parents]
    # core/ui_native/winui.cb is the WinUI 3 host: it imports the Windows App SDK runtime
    # winmds (Microsoft.UI.Xaml.winmd) via package-nuget, which the bare sweep cannot resolve
    # (no -i dir, no App SDK). It is exercised by example.bat's --worker-winui gate with the
    # right flags; skip it here like the example/ui/winui/* launchers above.
    # Path-qualified (not bare "winui.cb") since core/ui_native/ is not the only place
    # a file could plausibly be named winui.cb.
    files = [
        f for f in files
        if f.relative_to(REPO_ROOT).as_posix() != "cflat/core/ui_native/winui.cb"
    ]
    # gallery_app.cb is the host-neutral gallery component: it imports only ui.cb by
    # design and resolves the native* host drivers from whichever LAUNCHER imports it
    # (gallery.cb or winui_gallery.cb share one global scope with it). Standalone
    # analysis therefore cannot resolve those drivers; both launchers are swept and
    # example.bat exercises the component on both hosts. Skip it here.
    files = [f for f in files if f.name != "gallery_app.cb"]
    # todo_app.cb (example/ui/testing/) is the ui_test.cb template's host-neutral app module:
    # like gallery_app.cb it imports only the element model and resolves the native* host
    # drivers (hostWidth/hostHeight/...) from its LAUNCHER, todo_test.cb, which shares one
    # global scope with it. Standalone analysis cannot resolve those drivers; todo_test.cb is
    # swept and example.bat's --worker-uitest exercises the module. Skip it here.
    files = [f for f in files if f.name != "todo_app.cb"]
    return files


def run_bulk(exe: str, extra_args: list, show_timings: bool = False,
             include_win32_demo: bool = True) -> bool:
    paths = collect_files(include_win32_demo)
    skipped_win: list[Path] = []
    if not HOST_IS_WINDOWS:
        skipped_win = [p for p in paths if is_windows_only(p)]
        paths = [p for p in paths if not is_windows_only(p)]
    skipped_mac: list[Path] = []
    if not HOST_IS_MACOS:
        skipped_mac = [p for p in paths if is_macos_only(p)]
        paths = [p for p in paths if not is_macos_only(p)]
    print(f"Server: {exe}")
    print(f"Files:  {len(paths)}")
    if skipped_win:
        print(f"Skipped: {len(skipped_win)} Windows/env-only sources (non-Windows host)")
    if skipped_mac:
        print(f"Skipped: {len(skipped_mac)} macOS-only sources (non-macOS host)")

    # Pipeline depth - open this many files concurrently, then wait for diagnostics.
    # The server pool runs analyses in parallel up to its hardware_concurrency limit,
    # so a depth a bit larger than the pool keeps the pipeline full.
    pipeline = max(8, (os.cpu_count() or 4))
    print(f"Pipeline depth: {pipeline}\n")

    client = LspClient(exe, extra_args)
    started_at = time.monotonic()
    try:
        initialize(client)

        passed = 0
        failed = 0
        failures: list[tuple[Path, str]] = []
        total = len(paths)

        # uri -> (index, path, submit_time, text)
        in_flight: dict[str, tuple[int, Path, float, str]] = {}
        next_idx = 0
        # (path, seconds) for every completed file, for an end-of-run summary.
        timings: list[tuple[Path, float]] = []
        hint_total = 0

        def submit_one() -> bool:
            nonlocal next_idx
            while next_idx < total:
                i = next_idx
                next_idx += 1
                path = paths[i]
                try:
                    text = path.read_text(encoding="utf-8")
                except Exception as e:
                    print(f"  FAIL  [{i+1:>3}/{total}] {path.relative_to(REPO_ROOT)}  (read error: {e})")
                    nonlocal_failed_inc(e, path)
                    continue
                uri = path.resolve().as_uri()
                in_flight[uri] = (i, path, time.monotonic(), text)
                client.notify("textDocument/didOpen", {
                    "textDocument": {
                        "uri":        uri,
                        "languageId": "cflat",
                        "version":    1,
                        "text":       text,
                    }
                })
                return True
            return False

        # Helper that mutates outer counters; declared as a closure to avoid passing
        # everything around. Python's nonlocal doesn't reach across this lambda-style
        # helper, so use list-cells.
        _counters = {"failed": 0}
        def nonlocal_failed_inc(e, path):
            _counters["failed"] += 1
            failures.append((path, f"read error: {e}"))

        # Prime the pipeline.
        for _ in range(pipeline):
            if not submit_one(): break

        # Drain.
        while in_flight:
            notif = client.wait_notification("textDocument/publishDiagnostics", timeout=DIAG_TIMEOUT)
            uri = notif["params"]["uri"]
            if uri not in in_flight:
                continue  # stale or unrelated publish
            i, path, submitted_at, text = in_flight.pop(uri)
            secs = time.monotonic() - submitted_at
            timings.append((path, secs))
            diags = notif["params"]["diagnostics"]
            hint_total += sum(1 for d in diags if d.get("severity", 1) == HINT_SEVERITY)
            errors = [d for d in diags if d.get("severity", 1) == ERROR_SEVERITY]
            misattributed = find_misattributed_hints(text, diags)
            if errors or misattributed:
                reasons = []
                if errors:
                    reasons.append(f"{len(errors)} error diagnostic(s)")
                if misattributed:
                    reasons.append(f"{len(misattributed)} misattributed hint(s)")
                print(f"  FAIL  [{i+1:>3}/{total}] {path.relative_to(REPO_ROOT)}  ({secs:6.2f}s, {', '.join(reasons)})")
                failed += 1
                parts = [
                    f"L{d.get('range', {}).get('start', {}).get('line', '?')+1}: {d.get('message','')}"
                    for d in errors[:3]
                ]
                parts += misattributed[:3]
                failures.append((path, " | ".join(parts)))
            else:
                print(f"  PASS  [{i+1:>3}/{total}] {path.relative_to(REPO_ROOT)}  ({secs:6.2f}s)")
                passed += 1
            client.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
            # Refill the pipeline.
            submit_one()

        failed += _counters["failed"]
        elapsed = time.monotonic() - started_at
        print(f"\n{passed} passed, {failed} failed in {elapsed:.1f}s")
        # Informational: legitimate "unused"/unreachable hints are expected across
        # the tree and are not failures - only misattributed ones (above) are.
        print(f"({hint_total} faded hint(s) seen across all files)")

        # Per-file wall-clock latency (submit -> diagnostics). Files run concurrently
        # in the pipeline, so these include queueing time, not pure analysis cost.
        # Opt-in via --timings; off by default to keep the normal sweep output terse.
        if show_timings and timings:
            print("\n--- slowest files (wall-clock latency, includes pipeline queueing) ---")
            for p, secs in sorted(timings, key=lambda t: t[1], reverse=True)[:15]:
                print(f"  {secs:6.2f}s  {p.relative_to(REPO_ROOT)}")

        if failures:
            print("\n--- failures ---")
            for p, msg in failures:
                print(f"  {p.relative_to(REPO_ROOT)}")
                print(f"      {msg}")
        return failed == 0
    finally:
        try:
            client.request("shutdown")
            client.notify("exit")
        except Exception:
            pass
        stderr = client.close()
        if stderr.strip():
            print("\n--- server stderr ---")
            try:
                print(stderr.strip())
            except UnicodeEncodeError:
                sys.stdout.buffer.write(stderr.strip().encode("utf-8", errors="replace"))
                sys.stdout.buffer.write(b"\n")


def main():
    # --timings opts into the "slowest files" summary; it is not an LSP server arg.
    argv = sys.argv[1:]
    show_timings = "--timings" in argv
    argv = [a for a in argv if a != "--timings"]

    extra_args: list[str] = []
    exe: str | None = None
    if argv:
        first = argv[0]
        # Accept either a Windows cflat.exe or an extensionless POSIX cflat binary.
        if not first.startswith("-") and Path(first).exists():
            exe = first
            extra_args = argv[1:]
        else:
            extra_args = argv
    if exe is None:
        exe = find_exe()
        if exe is None:
            print("error: cflat.exe not found. Build first or pass the path.", file=sys.stderr)
            sys.exit(1)

    # Resolve example/COM/direct2d_demo.cb's "Windows.Win32.winmd" import by pointing -i at the
    # Win32-metadata NuGet package. If it isn't installed, drop that one file from the sweep so a
    # bare host stays green rather than failing on an import it cannot satisfy.
    win32_dir = find_win32_metadata_dir()
    if win32_dir is not None:
        extra_args = extra_args + ["-i", str(win32_dir)]
        print(f"Win32 metadata: {win32_dir}")
    else:
        print(f"Win32 metadata: not installed - skipping {WIN32_METADATA_DEMO}")

    # fedit imports its framework from example/ui; a second -i lets it resolve alongside
    # the Win32-metadata dir (import dirs are searched in order, first match wins).
    ui_dir = (REPO_ROOT / UI_FRAMEWORK_DIR).resolve()
    extra_args = extra_args + ["-i", str(ui_dir)]

    # The macOS NativeHost demos (cocoa_native_host/settings) import "cocoa.cb" from
    # example/macos; add it so those resolve on a Mac (harmless extra dir on Windows).
    macos_dir = (REPO_ROOT / "example" / "macos").resolve()
    if macos_dir.is_dir():
        extra_args = extra_args + ["-i", str(macos_dir)]

    ok = run_bulk(exe, extra_args, show_timings, include_win32_demo=win32_dir is not None)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
