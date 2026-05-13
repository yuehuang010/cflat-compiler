#!/usr/bin/env python3
"""
Bulk LSP test: open every real .cb source file in the repo through the
LSP server and assert that no Error-severity diagnostics are reported.

Files swept:
  - Test/*.cb           (top-level test programs)
  - example/**/*.cb     (recursive)
  - cflat/core/*.cb     (standard library, excluding runtime.cb)

Usage:
    python vscode-extension/test/lsp_bulk_test.py [path/to/cflat.exe] [extra lsp args]

Exit code: 0 = every file clean, 1 = at least one file produced an error
diagnostic (or the server died / timed out).
"""

import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lsp_client import LspClient, find_exe, initialize, wait_diagnostics_for

REPO_ROOT = Path(__file__).parent.parent.parent

# DiagnosticSeverity: 1=Error, 2=Warning, 3=Info, 4=Hint
ERROR_SEVERITY = 1

# Per-file diagnostics timeout (seconds). Some files (heavy generics) are slow.
DIAG_TIMEOUT = 300.0


def collect_files() -> list[Path]:
    files: list[Path] = []
    files += sorted((REPO_ROOT / "Test").glob("*.cb"))
    files += sorted((REPO_ROOT / "example").rglob("*.cb"))
    files += sorted(
        p for p in (REPO_ROOT / "cflat" / "core").glob("*.cb")
        if p.name != "runtime.cb"
    )
    return files


def run_bulk(exe: str, extra_args: list) -> bool:
    paths = collect_files()
    print(f"Server: {exe}")
    print(f"Files:  {len(paths)}")

    # Pipeline depth — open this many files concurrently, then wait for diagnostics.
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

        # uri -> (index, path)
        in_flight: dict[str, tuple[int, Path]] = {}
        next_idx = 0

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
                in_flight[uri] = (i, path)
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
            i, path = in_flight.pop(uri)
            diags = notif["params"]["diagnostics"]
            errors = [d for d in diags if d.get("severity", 1) == ERROR_SEVERITY]
            if errors:
                print(f"  FAIL  [{i+1:>3}/{total}] {path.relative_to(REPO_ROOT)}  ({len(errors)} error diagnostic(s))")
                failed += 1
                summary = " | ".join(
                    f"L{d.get('range', {}).get('start', {}).get('line', '?')+1}: {d.get('message','')}"
                    for d in errors[:3]
                )
                failures.append((path, summary))
            else:
                print(f"  PASS  [{i+1:>3}/{total}] {path.relative_to(REPO_ROOT)}")
                passed += 1
            client.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
            # Refill the pipeline.
            submit_one()

        failed += _counters["failed"]
        elapsed = time.monotonic() - started_at
        print(f"\n{passed} passed, {failed} failed in {elapsed:.1f}s")
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
    extra_args: list[str] = []
    exe: str | None = None
    if len(sys.argv) >= 2:
        first = sys.argv[1]
        if Path(first).exists() and first.lower().endswith(".exe"):
            exe = first
            extra_args = sys.argv[2:]
        else:
            extra_args = sys.argv[1:]
    if exe is None:
        exe = find_exe()
        if exe is None:
            print("error: cflat.exe not found. Build first or pass the path.", file=sys.stderr)
            sys.exit(1)

    ok = run_bulk(exe, extra_args)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
