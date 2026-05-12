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

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lsp_client import LspClient, find_exe, initialize, wait_diagnostics_for

REPO_ROOT = Path(__file__).parent.parent.parent

# DiagnosticSeverity: 1=Error, 2=Warning, 3=Info, 4=Hint
ERROR_SEVERITY = 1

# Per-file diagnostics timeout (seconds). Some files (heavy generics) are slow.
DIAG_TIMEOUT = 60.0


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
    print(f"Files:  {len(paths)}\n")

    client = LspClient(exe, extra_args)
    try:
        initialize(client)

        passed = 0
        failed = 0
        failures: list[tuple[Path, str]] = []

        for i, path in enumerate(paths, 1):
            uri = path.resolve().as_uri()
            try:
                text = path.read_text(encoding="utf-8")
            except Exception as e:
                print(f"  FAIL  [{i:>3}/{len(paths)}] {path.relative_to(REPO_ROOT)}  (read error: {e})")
                failed += 1
                failures.append((path, f"read error: {e}"))
                continue

            client.notify("textDocument/didOpen", {
                "textDocument": {
                    "uri":        uri,
                    "languageId": "cflat",
                    "version":    1,
                    "text":       text,
                }
            })

            try:
                diags = wait_diagnostics_for(client, uri, timeout=DIAG_TIMEOUT)
            except (TimeoutError, RuntimeError) as e:
                print(f"  FAIL  [{i:>3}/{len(paths)}] {path.relative_to(REPO_ROOT)}  ({e})")
                failed += 1
                failures.append((path, str(e)))
                # If the server died we can't continue meaningfully.
                if isinstance(e, RuntimeError):
                    break
                continue

            errors = [d for d in diags if d.get("severity", 1) == ERROR_SEVERITY]
            if errors:
                print(f"  FAIL  [{i:>3}/{len(paths)}] {path.relative_to(REPO_ROOT)}  ({len(errors)} error diagnostic(s))")
                failed += 1
                summary = " | ".join(
                    f"L{d.get('range', {}).get('start', {}).get('line', '?')+1}: {d.get('message','')}"
                    for d in errors[:3]
                )
                failures.append((path, summary))
            else:
                print(f"  PASS  [{i:>3}/{len(paths)}] {path.relative_to(REPO_ROOT)}")
                passed += 1

            client.notify("textDocument/didClose", {
                "textDocument": {"uri": uri}
            })

        print(f"\n{passed} passed, {failed} failed")
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
