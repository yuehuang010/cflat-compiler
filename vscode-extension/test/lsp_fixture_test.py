#!/usr/bin/env python3
"""
Fixture-driven LSP tests for cflat.exe lsp.

Each .cb file in Test/lsp/fixtures/ may contain directive comments:
    // $cursor line=N col=N        -- LSP position for hover/definition/completion
    // $expect hover contains="X"  -- hover markdown contains substring X
    // $expect hover null           -- hover returns null (nothing at cursor)
    // $expect definition line=N   -- definition result is at line N (0-based)
    // $expect completion includes="X" -- completion list contains label X
    // $expect diagnostic message="X"  -- at least one diagnostic message contains X
    // $expect no_diagnostic           -- publishDiagnostics has empty list

Directives are stripped before sending source to the LSP server, so the
line/col in $cursor refers to positions in the stripped source.

Usage:
    python vscode-extension/test/lsp_fixture_test.py [path/to/cflat.exe]

Exit code: 0 = all passed, 1 = one or more failures.
"""

import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from lsp_client import LspClient, find_exe, initialize, wait_diagnostics_for

REPO_ROOT = Path(__file__).parent.parent.parent
FIXTURE_DIR = REPO_ROOT / "cflat" / "test_lsp" / "fixtures"

# Virtual URI prefix - server analyzes via temp files internally, so these
# paths don't need to exist on disk.
_URI_BASE = "file:///C%3A/lsp_fixture_"


def _uri_for(name: str) -> str:
    return _URI_BASE + name + ".cb"


# ---------------------------------------------------------------------------
# Directive parser
# ---------------------------------------------------------------------------

_CURSOR_RE  = re.compile(r"//\s+\$cursor\s+line=(\d+)\s+col=(\d+)")
_EXPECT_RE  = re.compile(r"//\s+\$expect\s+(.*)")


def _parse_kv(text: str) -> dict:
    """Extract key="value" and key=N pairs from a string."""
    result = {}
    for m in re.finditer(r'(\w+)="([^"]*)"', text):
        result[m.group(1)] = m.group(2)
    for m in re.finditer(r'(\w+)=(\d+)', text):
        if m.group(1) not in result:
            result[m.group(1)] = int(m.group(2))
    return result


def parse_fixture(content: str) -> tuple[dict, str]:
    """Return (directives, stripped_source).

    directives keys:
      'cursor'  -> (line, col) tuple
      'expect'  -> (kind, kv_dict) tuple
    """
    directives: dict = {}
    source_lines: list[str] = []

    for line in content.splitlines(keepends=True):
        stripped = line.strip()
        m = _CURSOR_RE.match(stripped)
        if m:
            directives["cursor"] = (int(m.group(1)), int(m.group(2)))
            continue
        m = _EXPECT_RE.match(stripped)
        if m:
            rest = m.group(1).strip()
            parts = rest.split(None, 1)
            kind = parts[0]
            kv = _parse_kv(parts[1]) if len(parts) > 1 else {}
            directives["expect"] = (kind, kv)
            continue
        source_lines.append(line)

    return directives, "".join(source_lines)


# ---------------------------------------------------------------------------
# Fixture runner
# ---------------------------------------------------------------------------

def _open_doc(client: LspClient, uri: str, source: str, version: int = 1):
    client.notify("textDocument/didOpen", {
        "textDocument": {
            "uri": uri,
            "languageId": "cflat",
            "version": version,
            "text": source,
        }
    })


def run_fixture(client: LspClient, fixture_path: Path) -> str | None:
    """Return None on pass, error description on failure."""
    content = fixture_path.read_text(encoding="utf-8")
    directives, source = parse_fixture(content)

    name = fixture_path.stem
    uri = _uri_for(name)

    expect_kind, expect_kv = directives.get("expect", (None, {}))
    cursor = directives.get("cursor", (0, 0))

    # Open document - triggers immediate analysis.
    _open_doc(client, uri, source)

    # For diagnostic expectations we need the publishDiagnostics notification.
    if expect_kind in ("diagnostic", "no_diagnostic"):
        diags = wait_diagnostics_for(client, uri)
        if expect_kind == "no_diagnostic":
            if diags:
                return f"expected no diagnostics, got: {diags}"
            return None
        # diagnostic message="X"
        needle = expect_kv.get("message", "")
        messages = [d.get("message", "") for d in diags]
        if not any(needle in msg for msg in messages):
            return f"expected diagnostic containing {needle!r}, got: {messages}"
        return None

    # Wait for analysis to finish so the symbol index is populated.
    wait_diagnostics_for(client, uri)

    line, col = cursor
    position = {"line": line, "character": col}

    if expect_kind == "hover_null":
        # $expect hover_null - cursor is on non-symbol text; hover must return null.
        resp = client.request("textDocument/hover", {
            "textDocument": {"uri": uri},
            "position": position,
        })
        if "result" not in resp:
            return f"hover_null: no result in response: {resp}"
        if resp["result"] is not None:
            return f"hover_null: expected null, got: {resp['result']}"
        return None

    if expect_kind == "hover":
        resp = client.request("textDocument/hover", {
            "textDocument": {"uri": uri},
            "position": position,
        })
        if "result" not in resp:
            return f"hover: no result in response: {resp}"
        result = resp["result"]
        if result is None:
            return f"hover: got null - symbol not found at ({line},{col})"
        value = result.get("contents", {}).get("value", "")
        needle = expect_kv.get("contains", "")
        if needle and needle not in value:
            return f"hover: expected {needle!r} in markdown, got: {value!r}"
        absent = expect_kv.get("not_contains", "")
        if absent and absent in value:
            return f"hover: did not expect {absent!r} in markdown, got: {value!r}"
        return None

    if expect_kind == "definition":
        resp = client.request("textDocument/definition", {
            "textDocument": {"uri": uri},
            "position": position,
        })
        if "result" not in resp:
            return f"definition: no result in response: {resp}"
        results = resp["result"]
        if not results:
            return f"definition: empty result - symbol not found at ({line},{col})"
        target_line = results[0].get("range", {}).get("start", {}).get("line")
        expected_line = expect_kv.get("line")
        if expected_line is not None and target_line != expected_line:
            return f"definition: expected line {expected_line}, got line {target_line}"
        return None

    if expect_kind == "completion":
        resp = client.request("textDocument/completion", {
            "textDocument": {"uri": uri},
            "position": position,
        })
        if "result" not in resp:
            return f"completion: no result in response: {resp}"
        result = resp["result"]
        items = result if isinstance(result, list) else result.get("items", [])
        labels = [item.get("label", "") for item in items]
        needle = expect_kv.get("includes", "")
        if needle and not any(needle in label for label in labels):
            return f"completion: expected label containing {needle!r}, got: {labels[:10]}"
        return None

    return f"unknown expect kind: {expect_kind!r}"


# ---------------------------------------------------------------------------
# Hardcoded scenario tests
# ---------------------------------------------------------------------------

def test_diagnostic_lifecycle(client: LspClient) -> str | None:
    """Error appears on open, then clears after fixing the source."""
    error_source = "extern int main() { return unknownVar; }"
    fixed_source = "extern int main() { return 0; }"
    uri = _uri_for("lifecycle")

    _open_doc(client, uri, error_source)
    diags = wait_diagnostics_for(client, uri)
    if not diags:
        return "lifecycle: expected diagnostic for error source, got none"

    # Fix via didChange + didSave (didSave triggers immediate re-analysis).
    client.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 2},
        "contentChanges": [{"text": fixed_source}],
    })
    client.notify("textDocument/didSave", {
        "textDocument": {"uri": uri},
        "text": fixed_source,
    })
    diags = wait_diagnostics_for(client, uri)
    if diags:
        return f"lifecycle: expected diagnostics to clear after fix, got: {diags}"
    return None


def test_def_non_primitives(client: LspClient) -> str | None:
    """Go-to-definition covers every non-primitive kind: namespace, struct, local var type, local var name."""
    # Source has no indentation so column positions are trivial to count.
    # Line numbers (0-based):
    #  0  namespace Geometry {
    #  1  struct Point {
    #  2  int x = 0;
    #  3  int y = 0;
    #  4  };
    #  5  struct Circle {
    #  6  int cx = 0;
    #  7  int cy = 0;
    #  8  int radius = 0;
    #  9  };
    # 10  }
    # 11  extern int main() {
    # 12  Geometry.Point p;
    # 13  Geometry.Circle c;
    # 14  return 0;
    # 15  }
    source = (
        "namespace Geometry {\n"
        "struct Point {\n"
        "int x = 0;\n"
        "int y = 0;\n"
        "};\n"
        "struct Circle {\n"
        "int cx = 0;\n"
        "int cy = 0;\n"
        "int radius = 0;\n"
        "};\n"
        "}\n"
        "extern int main() {\n"
        "Geometry.Point p;\n"
        "Geometry.Circle c;\n"
        "return 0;\n"
        "}\n"
    )
    uri = _uri_for("def_non_primitives")
    _open_doc(client, uri, source)
    wait_diagnostics_for(client, uri)

    # Each entry: (description, line, col)
    cases = [
        # Declarations
        ("namespace Geometry (decl)",   0, 10),  # 'G' of Geometry
        ("struct Point (decl)",         1,  7),  # 'P' of Point
        ("struct Circle (decl)",        5,  7),  # 'C' of Circle
        # Usage as local variable type
        ("Point type in 'Geometry.Point p'",  12,  9),  # 'P' of Point after 'Geometry.'
        ("Circle type in 'Geometry.Circle c'", 13,  9),  # 'C' of Circle after 'Geometry.'
        # Variable names (cursor on the variable identifier itself)
        ("local var 'p' (struct type)",  12, 15),  # 'p' after 'Geometry.Point '
        ("local var 'c' (struct type)",  13, 16),  # 'c' after 'Geometry.Circle '
    ]

    failures = []
    for desc, line, col in cases:
        resp = client.request("textDocument/definition", {
            "textDocument": {"uri": uri},
            "position": {"line": line, "character": col},
        })
        results = resp.get("result") or []
        if not results:
            failures.append(f"{desc} (line={line}, col={col}): no definition returned")

    if not failures:
        return None
    return "\n        ".join(failures)


def test_def_nested_struct(client: LspClient) -> str | None:
    """Go-to-definition covers nested struct/class declarations and their usages."""
    # Source has no indentation so column positions are trivial to count.
    # Line numbers (0-based):
    #  0  struct OuterStruct {
    #  1  struct InnerStruct {
    #  2  int x = 0;
    #  3  int y = 0;
    #  4  };
    #  5  InnerStruct inner;
    #  6  int value = 0;
    #  7  };
    #  8  extern int main() {
    #  9  OuterStruct o;
    # 10  OuterStruct.InnerStruct standalone;
    # 11  int v = o.inner.x;
    # 12  return 0;
    # 13  }
    source = (
        "struct OuterStruct {\n"
        "struct InnerStruct {\n"
        "int x = 0;\n"
        "int y = 0;\n"
        "};\n"
        "InnerStruct inner;\n"
        "int value = 0;\n"
        "};\n"
        "extern int main() {\n"
        "OuterStruct o;\n"
        "OuterStruct.InnerStruct standalone;\n"
        "int v = o.inner.x;\n"
        "return 0;\n"
        "}\n"
    )
    uri = _uri_for("def_nested_struct")
    _open_doc(client, uri, source)
    wait_diagnostics_for(client, uri)

    # Cases that must return a definition result.
    must_find = [
        # Nested type name as field type: "InnerStruct inner;"
        ("InnerStruct type in field decl",           5,  0),  # 'I' of InnerStruct
        # Nested type in qualified standalone var: "OuterStruct.InnerStruct standalone;"
        ("InnerStruct in qualified var decl",        10, 12),  # 'I' of InnerStruct after 'OuterStruct.'
        # Variable name whose type is a nested struct
        ("local var 'o' (OuterStruct type)",          9,  0),  # 'o' resolves via variableTypes
        # Field access: "o.inner" - navigate to the 'inner' field declaration
        ("field access 'o.inner'",                   11, 10),  # 'i' of inner after 'int v = o.'
    ]

    # Cases where the cursor is already at the definition - must return null.
    must_be_null = [
        # Field name at its own declaration site
        ("field name 'inner' at its own decl",        5, 12),  # 'i' of inner after 'InnerStruct '
    ]

    failures = []

    for desc, line, col in must_find:
        resp = client.request("textDocument/definition", {
            "textDocument": {"uri": uri},
            "position": {"line": line, "character": col},
        })
        results = resp.get("result") or []
        if not results:
            failures.append(f"{desc} (line={line}, col={col}): no definition returned")

    for desc, line, col in must_be_null:
        resp = client.request("textDocument/definition", {
            "textDocument": {"uri": uri},
            "position": {"line": line, "character": col},
        })
        results = resp.get("result") or []
        if results:
            target_line = results[0].get("range", {}).get("start", {}).get("line")
            failures.append(f"{desc} (line={line}, col={col}): expected null, got line {target_line}")

    if not failures:
        return None
    return "\n        ".join(failures)


def test_reanalysis_state_isolation(exe: str) -> str | None:
    """Regression for L1: transient/module-bound backend state must not leak across a backend
    reanalysis. With the pool forced to a single slot, file B is always analyzed on the same
    LLVMBackend that just analyzed file A, so any state left set by A that survives
    ResetForReanalysis corrupts B's analysis. The confirmed L1 root cause is a stale
    fullDestructorCache_ entry: a full-destructor llvm::Function* synthesized while analyzing A
    lives in A's module; if it survives the reset, B's analysis emits a call to that Function*
    from A's now-freed module -> "Internal compiler error during analysis" on B.

    file_a (Test/test_threadpool.cb) then file_b (Test/test_parallel.cb) is the minimal
    deterministic repro: both pass standalone (they are part of test.bat), but with the cache
    clear removed B reliably fails on this exact ordering. B-after-A must equal B-alone."""
    # Pool size 1 => one backend slot => B is guaranteed to reanalyze the slot A just used.
    saved = os.environ.get("CFLAT_LSP_POOL_SIZE")
    os.environ["CFLAT_LSP_POOL_SIZE"] = "1"
    try:
        client = LspClient(exe)
    finally:
        if saved is None:
            os.environ.pop("CFLAT_LSP_POOL_SIZE", None)
        else:
            os.environ["CFLAT_LSP_POOL_SIZE"] = saved

    path_a = REPO_ROOT / "Test" / "test_threadpool.cb"
    path_b = REPO_ROOT / "Test" / "test_parallel.cb"
    if not path_a.exists() or not path_b.exists():
        return f"reanalysis: repro files missing ({path_a}, {path_b})"
    try:
        initialize(client)
        uri_a = path_a.resolve().as_uri()
        uri_b = path_b.resolve().as_uri()

        _open_doc(client, uri_a, path_a.read_text(encoding="utf-8"))
        diags_a = wait_diagnostics_for(client, uri_a, timeout=60.0)
        errors_a = [d for d in diags_a if d.get("severity", 1) == 1]
        if errors_a:
            msgs = [d.get("message", "") for d in errors_a]
            return f"reanalysis: file A not clean standalone (test fixture stale): {msgs}"

        _open_doc(client, uri_b, path_b.read_text(encoding="utf-8"))
        diags_b = wait_diagnostics_for(client, uri_b, timeout=60.0)
        errors_b = [d for d in diags_b if d.get("severity", 1) == 1]
        if errors_b:
            msgs = [d.get("message", "") for d in errors_b]
            return f"reanalysis: file B (clean standalone) reported errors after A: {msgs}"
        return None
    except (TimeoutError, RuntimeError) as e:
        return f"reanalysis: server failed during B-after-A: {e}"
    finally:
        try:
            client.request("shutdown")
            client.notify("exit")
        except Exception:
            pass
        client.close()


def test_negative_hover_before_initialize(exe: str) -> str | None:
    """Hover before initialize should return an error response, not crash."""
    client = LspClient(exe)
    try:
        resp = client.request("textDocument/hover", {
            "textDocument": {"uri": _uri_for("preinit")},
            "position": {"line": 0, "character": 0},
        }, timeout=5.0)
        # Server should respond with an error or null result - not crash.
        if "error" not in resp and resp.get("result") is None:
            return None  # null result is acceptable
        if "error" in resp:
            return None  # error response is correct
        return f"negative: unexpected response to pre-init hover: {resp}"
    except TimeoutError:
        return "negative: server timed out on pre-init hover"
    finally:
        client.close()


def test_server_resilience(client: LspClient) -> str | None:
    """Server stays alive after receiving a document that triggers an analysis error."""
    # A file with a crash-prone pattern: deep nesting / invalid construct.
    # Even if the compiler errors out, the server should keep responding.
    bad_source = "extern int main() { int x = ((((((((((0)))))))))))))))); }"
    uri = _uri_for("resilience")

    _open_doc(client, uri, bad_source)
    # Don't care about diagnostics content - just drain the notification.
    try:
        wait_diagnostics_for(client, uri, timeout=10.0)
    except TimeoutError:
        pass  # server may not publish diagnostics for a crash - that's OK

    # Verify server still responds to a subsequent hover request.
    try:
        resp = client.request("textDocument/hover", {
            "textDocument": {"uri": _uri_for("hover_function")},
            "position": {"line": 0, "character": 11},
        }, timeout=10.0)
        if "result" not in resp and "error" not in resp:
            return "resilience: server stopped responding after bad input"
    except (TimeoutError, RuntimeError) as e:
        return f"resilience: server died after bad input: {e}"
    return None


# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------

def run_all(exe: str, extra_args: list) -> bool:
    if not FIXTURE_DIR.exists():
        print(f"error: fixture directory not found: {FIXTURE_DIR}", file=sys.stderr)
        return False

    fixtures = sorted(FIXTURE_DIR.glob("*.cb"))
    print(f"Server:   {exe}")
    print(f"Fixtures: {FIXTURE_DIR} ({len(fixtures)} files)\n")

    client = LspClient(exe, extra_args)
    initialize(client)

    passed = 0
    failed = 0

    def record(name: str, error: str | None):
        nonlocal passed, failed
        if error is None:
            print(f"  PASS  {name}")
            passed += 1
        else:
            print(f"  FAIL  {name}")
            print(f"        {error}")
            failed += 1

    # --- fixture-driven tests ---
    for fixture_path in fixtures:
        name = fixture_path.name
        try:
            err = run_fixture(client, fixture_path)
        except TimeoutError as e:
            err = f"TIMEOUT: {e}"
        except Exception as e:
            err = f"EXCEPTION: {e}"
        record(name, err)

    # --- hardcoded scenario tests ---
    try:
        record("diagnostic lifecycle", test_diagnostic_lifecycle(client))
    except Exception as e:
        record("diagnostic lifecycle", f"EXCEPTION: {e}")

    try:
        record("server resilience", test_server_resilience(client))
    except Exception as e:
        record("server resilience", f"EXCEPTION: {e}")

    try:
        record("def: non-primitives (namespace/struct/local var)", test_def_non_primitives(client))
    except Exception as e:
        record("def: non-primitives (namespace/struct/local var)", f"EXCEPTION: {e}")

    try:
        record("def: nested struct/class", test_def_nested_struct(client))
    except Exception as e:
        record("def: nested struct/class", f"EXCEPTION: {e}")

    # Shutdown main client.
    try:
        client.request("shutdown")
        client.notify("exit")
    except Exception:
        pass
    stderr = client.close()

    print(f"\n{passed} passed, {failed} failed")
    if stderr.strip():
        print("\n--- server stderr ---")
        print(stderr.strip())

    # --- out-of-process tests (each needs its own client) ---
    try:
        err = test_negative_hover_before_initialize(exe)
        record("negative: hover before initialize", err)
    except Exception as e:
        record("negative: hover before initialize", f"EXCEPTION: {e}")

    try:
        err = test_reanalysis_state_isolation(exe)
        record("reanalysis: B-after-A state isolation (L1)", err)
    except Exception as e:
        record("reanalysis: B-after-A state isolation (L1)", f"EXCEPTION: {e}")

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
                "error: cflat.exe not found. Build first, or pass the path as an argument.",
                file=sys.stderr,
            )
            sys.exit(1)

    ok = run_all(exe, extra_args)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
