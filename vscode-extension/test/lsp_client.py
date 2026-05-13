"""
Shared LSP client for MyCompiler LSP tests.
Spawns cflat.exe lsp and speaks JSON-RPC over its stdio.
"""

import collections
import json
import os
import subprocess
import threading
import time
from pathlib import Path


class LspClient:
    """Spawns cflat.exe lsp and speaks LSP over its stdio."""

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
        self._stderr_buf: list[bytes] = []
        t = threading.Thread(target=self._reader_loop, daemon=True)
        t.start()
        # Drain stderr concurrently so the server doesn't block on a full pipe buffer
        # when many workers write diagnostics at once.
        e = threading.Thread(target=self._stderr_drain, daemon=True)
        e.start()

    def _stderr_drain(self):
        try:
            while True:
                chunk = self._proc.stderr.read(4096)
                if not chunk:
                    return
                self._stderr_buf.append(chunk)
        except Exception:
            pass

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
            self._proc.wait(timeout=5)
        except Exception:
            self._proc.kill()
        # Drain thread is daemon — give it a moment to finish.
        try:
            return b"".join(self._stderr_buf).decode("utf-8", errors="replace")
        except Exception:
            return ""


def find_exe() -> str | None:
    """Search standard build output locations for cflat.exe."""
    script_dir = Path(__file__).parent.resolve()
    repo_root = script_dir.parent.parent
    candidates = [
        repo_root / "x64" / "Release" / "cflat.exe",
        repo_root / "x64" / "Debug"   / "cflat.exe",
        repo_root / "x86" / "Release" / "cflat.exe",
        repo_root / "x86" / "Debug"   / "cflat.exe",
    ]
    for p in candidates:
        if p.exists():
            return str(p)
    return None


def wait_diagnostics_for(client: LspClient, uri: str, timeout: float = 20.0) -> list:
    """Keep reading publishDiagnostics until one matches the given URI."""
    while True:
        notif = client.wait_notification("textDocument/publishDiagnostics", timeout=timeout)
        if notif["params"]["uri"] == uri:
            return notif["params"]["diagnostics"]


def initialize(client: LspClient):
    """Send initialize + initialized handshake."""
    import os
    client.request("initialize", {
        "processId": os.getpid(),
        "rootUri": None,
        "capabilities": {},
    })
    client.notify("initialized", {})
