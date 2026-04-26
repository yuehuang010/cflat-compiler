#!/usr/bin/env python3
"""
Smoke test for MyCompiler.exe lsp -- runs without VS Code.

Usage (from repo root):
    python vscode-extension/test/lsp_test.py

Or pass the exe path explicitly:
    python vscode-extension/test/lsp_test.py path/to/MyCompiler.exe

Exit code: 0 = all passed, 1 = one or more failures.
"""

import collections
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

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
# Minimal LSP client (JSON-RPC over stdio with Content-Length framing)
# ---------------------------------------------------------------------------

class LspClient:
    """Spawns MyCompiler.exe lsp and speaks LSP over its stdio."""

    def __init__(self, exe: str, extra_args: list = ()):
        self._proc = subprocess.Popen(
            [exe, "lsp", *extra_args],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self._next_id = 1
        self._cond = threading.Condition()
        self._inbox: collections.deque = collections.deque()
        self._dead = False
        t = threading.Thread(target=self._reader_loop, daemon=True)
        t.start()

    # --- internal reader thread ---

    def _reader_loop(self):
        try:
            while True:
                msg = self._read_one()
                with self._cond:
                    self._inbox.append(msg)
                    self._cond.notify_all()
        except Exception:
            with self._cond:
                self._dead = True
                self._cond.notify_all()

    def _read_one(self) -> dict:
        headers: dict = {}
        while True:
            line = bytearray()
            while True:
                ch = self._proc.stdout.read(1)
                if not ch:
                    raise EOFError("server stdout closed")
                if ch == b"\r":
                    self._proc.stdout.read(1)  # consume \n
                    break
                line += ch
            text = line.decode("ascii").strip()
            if not text:
                break
            key, _, val = text.partition(":")
            headers[key.strip().lower()] = val.strip()
        length = int(headers["content-length"])
        body = self._proc.stdout.read(length)
        return json.loads(body.decode("utf-8"))

    # --- send helpers ---

    def _send(self, msg: dict):
        body = json.dumps(msg, separators=(",", ":")).encode("utf-8")
        frame = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body
        self._proc.stdin.write(frame)
        self._proc.stdin.flush()

    def notify(self, method: str, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)

    def request(self, method: str, params=None, timeout: float = 20.0) -> dict:
        req_id = self._next_id
        self._next_id += 1
        msg = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            msg["params"] = params
        self._send(msg)
        return self._wait(lambda m: m.get("id") == req_id, timeout, f"response to '{method}'")

    def wait_notification(self, method: str, timeout: float = 20.0) -> dict:
        return self._wait(lambda m: m.get("method") == method, timeout, f"notification '{method}'")

    def _wait(self, predicate, timeout: float, description: str) -> dict:
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                for i, msg in enumerate(self._inbox):
                    if predicate(msg):
                        del self._inbox[i]
                        return msg
                if self._dead:
                    raise RuntimeError(f"Server died while waiting for {description}")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"Timed out after {timeout}s waiting for {description}")
                self._cond.wait(timeout=min(remaining, 1.0))

    # --- teardown ---

    def close(self) -> str:
        """Shut down the process and return any stderr output."""
        try:
            self._proc.stdin.close()
        except Exception:
            pass
        try:
            stderr = self._proc.stderr.read().decode("utf-8", errors="replace")
        except Exception:
            stderr = ""
        try:
            self._proc.wait(timeout=5)
        except Exception:
            self._proc.kill()
        return stderr

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
    notif = client.wait_notification("textDocument/publishDiagnostics")
    if notif["params"]["uri"] != VALID_URI:
        # Could be a diagnostic from a different URI; wait for the right one
        notif = client.wait_notification("textDocument/publishDiagnostics")
    diags = notif["params"]["diagnostics"]
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
        return "hover returned null — symbol 'add' not found in index"
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
    # Wait for the diagnostic notification for our error file specifically
    while True:
        notif = client.wait_notification("textDocument/publishDiagnostics")
        if notif["params"]["uri"] == ERROR_URI:
            break
    diags = notif["params"]["diagnostics"]
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


def find_exe() -> str | None:
    script_dir = Path(__file__).parent.resolve()
    repo_root = script_dir.parent.parent
    candidates = [
        repo_root / "x64" / "Debug"   / "MyCompiler.exe",
        repo_root / "x64" / "Release" / "MyCompiler.exe",
        repo_root / "x86" / "Debug"   / "MyCompiler.exe",
        repo_root / "x86" / "Release" / "MyCompiler.exe",
    ]
    for p in candidates:
        if p.exists():
            return str(p)
    return None


def run_tests(exe: str) -> bool:
    print(f"Server: {exe}\n")

    client = LspClient(exe)
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
    if len(sys.argv) >= 2:
        exe = sys.argv[1]
        if not Path(exe).exists():
            print(f"error: not found: {exe}", file=sys.stderr)
            sys.exit(1)
    else:
        exe = find_exe()
        if exe is None:
            print(
                "error: MyCompiler.exe not found. Build the project first, or pass the path as an argument.",
                file=sys.stderr,
            )
            sys.exit(1)

    ok = run_tests(exe)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
