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


def collect_files(include_win32_demo: bool) -> list[Path]:
    files: list[Path] = []
    files += sorted((REPO_ROOT / "Test").glob("*.cb"))
    files += sorted((REPO_ROOT / "example").rglob("*.cb"))
    files += sorted(
        p for p in (REPO_ROOT / "cflat" / "core").glob("*.cb")
        if p.name != "runtime.cb" and p.name != NON_HOST_OS_FILE
    )
    if not include_win32_demo:
        demo = (REPO_ROOT / WIN32_METADATA_DEMO).resolve()
        files = [f for f in files if f.resolve() != demo]
    return files


def run_bulk(exe: str, extra_args: list, show_timings: bool = False,
             include_win32_demo: bool = True) -> bool:
    paths = collect_files(include_win32_demo)
    print(f"Server: {exe}")
    print(f"Files:  {len(paths)}")

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
        if Path(first).exists() and first.lower().endswith(".exe"):
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

    ok = run_bulk(exe, extra_args, show_timings, include_win32_demo=win32_dir is not None)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
