#!/usr/bin/env python3
"""
Smoke test for cflat.exe lsp -- runs without VS Code.

Usage (from repo root):
    python vscode-extension/test/lsp_test.py

Or pass the exe path explicitly:
    python vscode-extension/test/lsp_test.py path/to/cflat.exe

Exit code: 0 = all passed, 1 = one or more failures.
"""

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lsp_client import LspClient, find_exe, wait_diagnostics_for

# ---------------------------------------------------------------------------
# CFlat source snippets used by the tests
# ---------------------------------------------------------------------------

# A valid file that defines 'add' (line 0, col 11) and 'main'.
VALID_SOURCE = """\
extern int add(int a, int b) { return a + b; }
extern int main()
{
    return add(1, 2);
}
"""

# A file with a deliberate undefined-variable error.
ERROR_SOURCE = """\
extern int main()
{
    return unknownVariable;
}
"""

# ---------------------------------------------------------------------------
# Individual test functions
# Each returns None on success or a failure message string.
# ---------------------------------------------------------------------------

VALID_URI = "file:///C%3A/lsp_test_valid.cb"
ERROR_URI = "file:///C%3A/lsp_test_error.cb"


def test_initialize(client: LspClient):
    resp = client.request("initialize", {
        "processId": os.getpid(),
        "rootUri": None,
        "capabilities": {},
    })
    if "result" not in resp:
        return f"expected result, got: {resp}"
    caps = resp["result"].get("capabilities", {})
    missing = [c for c in ("hoverProvider", "definitionProvider") if not caps.get(c)]
    if missing:
        return f"missing capabilities: {missing}"
    client.notify("initialized", {})
    return None


def test_diagnostics_clean_file(client: LspClient):
    client.notify("textDocument/didOpen", {
        "textDocument": {
            "uri": VALID_URI,
            "languageId": "cflat",
            "version": 1,
            "text": VALID_SOURCE,
        }
    })
    diags = wait_diagnostics_for(client, VALID_URI)
    if diags:
        return f"expected no diagnostics for valid source, got: {diags}"
    return None


def test_hover(client: LspClient):
    # 'add' starts at line 0, column 11 in VALID_SOURCE
    # ("extern int " = 11 chars, then 'add')
    resp = client.request("textDocument/hover", {
        "textDocument": {"uri": VALID_URI},
        "position": {"line": 0, "character": 11},
    })
    if "result" not in resp:
        return f"expected result, got: {resp}"
    if resp["result"] is None:
        return "hover returned null - symbol 'add' not found in index"
    contents = resp["result"].get("contents", {})
    value = contents.get("value", "")
    if "add" not in value:
        return f"expected 'add' in hover markdown, got: {value!r}"
    return None


def test_diagnostics_error_file(client: LspClient):
    client.notify("textDocument/didOpen", {
        "textDocument": {
            "uri": ERROR_URI,
            "languageId": "cflat",
            "version": 1,
            "text": ERROR_SOURCE,
        }
    })
    diags = wait_diagnostics_for(client, ERROR_URI)
    if not diags:
        return "expected at least one diagnostic for error source"
    messages = " | ".join(d.get("message", "") for d in diags)
    if not any(kw in messages for kw in ("Undefined", "undefined", "unknownVariable")):
        return f"expected 'Undefined variable' in diagnostics, got: {messages!r}"
    return None


def test_shutdown(client: LspClient):
    resp = client.request("shutdown")
    if "result" not in resp:
        return f"shutdown request failed: {resp}"
    client.notify("exit")
    return None


# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------

TESTS = [
    ("initialize",               test_initialize),
    ("diagnostics: clean file",  test_diagnostics_clean_file),
    ("hover: symbol lookup",     test_hover),
    ("diagnostics: error file",  test_diagnostics_error_file),
    ("shutdown",                 test_shutdown),
]


def run_tests(exe: str, extra_args: list) -> bool:
    print(f"Server: {exe}\n")

    client = LspClient(exe, extra_args)
    passed = 0
    failed = 0

    for name, fn in TESTS:
        try:
            error = fn(client)
        except TimeoutError as e:
            error = f"TIMEOUT: {e}"
        except Exception as e:
            error = f"EXCEPTION: {e}"

        if error is None:
            print(f"  PASS  {name}")
            passed += 1
        else:
            print(f"  FAIL  {name}")
            print(f"        {error}")
            failed += 1

    stderr = client.close()
    print(f"\n{passed} passed, {failed} failed")
    if stderr.strip():
        print("\n--- server stderr ---")
        print(stderr.strip())
    return failed == 0


def main():
    extra_args: list = []
    if len(sys.argv) >= 2:
        exe = sys.argv[1]
        if not Path(exe).exists():
            print(f"error: not found: {exe}", file=sys.stderr)
            sys.exit(1)
        extra_args = sys.argv[2:]
    else:
        exe = find_exe()
        if exe is None:
            print(
                "error: cflat.exe not found. Build the project first, or pass the path as an argument.",
                file=sys.stderr,
            )
            sys.exit(1)

    ok = run_tests(exe, extra_args)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
