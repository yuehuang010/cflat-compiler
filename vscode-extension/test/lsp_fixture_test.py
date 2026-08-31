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
# Defer annotation evaluation so PEP 604 unions (str | None) run on Python 3.9.
from __future__ import annotations

import concurrent.futures
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

_CURSOR_RE   = re.compile(r"//\s+\$cursor\s+line=(\d+)\s+col=(\d+)")
_EXPECT_RE   = re.compile(r"//\s+\$expect\s+(.*)")
# `// $platform windows` marks a fixture that only runs on the named host (e.g. it
# imports windows.h). The suite runs the locally-built native cflat, so the host
# platform is also the target platform.
_PLATFORM_RE = re.compile(r"//\s+\$platform\s+(\w+)")

HOST_PLATFORM = ("windows" if sys.platform.startswith("win")
                 else "macos" if sys.platform == "darwin"
                 else "linux")


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
        m = _PLATFORM_RE.match(stripped)
        if m:
            directives["platform"] = m.group(1).lower()
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
    # os.environ is process-wide and this now runs alongside the shard threads, but every other
    # client passes an explicit --lsp-pool-size, which the server prefers over this env var.
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

def test_optimization_info(client: LspClient) -> str | None:
    """Check the Tier 1 per-function optimization facts behind the CodeLens."""
    source = (
        "int opt_info_helper(int value) {\n"
        "    return value * 3 + 1;\n"
        "}\n"
        "\n"
        "extern int main(int argc, char** argv) {\n"
        "    return opt_info_helper(argc);\n"
        "}\n"
    )
    uri = _uri_for("optimization_info")
    _open_doc(client, uri, source)
    diagnostics = wait_diagnostics_for(client, uri)
    if diagnostics:
        return f"optimizationInfo: unexpected diagnostics: {diagnostics}"

    counters = ("irInstructions", "machineInstructions", "bytes",
                "stackBytes", "spills", "reloads", "inlinedInto")

    def collect(opt_level: int):
        response = client.request("cflat/optimizationInfo",
                                  {"uri": uri, "optLevel": opt_level}, timeout=120.0)
        if "error" in response:
            return None, f"optimizationInfo O{opt_level}: unexpected error: {response}"
        result = response.get("result")
        if not isinstance(result, dict) or result.get("optLevel") != opt_level:
            return None, f"optimizationInfo O{opt_level}: bad result envelope: {response}"
        functions = result.get("functions")
        if not isinstance(functions, list) or not functions:
            return None, f"optimizationInfo O{opt_level}: no functions: {result}"
        by_name = {}
        for entry in functions:
            for field in ("name", "startLine", "endLine", "eliminated"):
                if field not in entry:
                    return None, f"optimizationInfo O{opt_level}: entry missing {field}: {entry}"
            for field in counters:
                if not isinstance(entry.get(field), int):
                    return None, f"optimizationInfo O{opt_level}: {field} not an int: {entry}"
            lines = entry.get("lines")
            if not isinstance(lines, list):
                return None, f"optimizationInfo O{opt_level}: lines not a list: {entry}"
            previous = 0
            for line in lines:
                for field in ("srcLine", "irInstructions", "machineInstructions"):
                    if not isinstance(line.get(field), int):
                        return None, (f"optimizationInfo O{opt_level}: line {field} "
                                      f"not an int: {line}")
                if not isinstance(line.get("inlined"), bool):
                    return None, f"optimizationInfo O{opt_level}: line inlined not a bool: {line}"
                if line["srcLine"] <= previous:
                    return None, (f"optimizationInfo O{opt_level}: lines not ascending "
                                  f"in {entry['name']}: {lines}")
                previous = line["srcLine"]
                if not (entry["startLine"] <= line["srcLine"] <= entry["endLine"]):
                    return None, (f"optimizationInfo O{opt_level}: line {line['srcLine']} "
                                  f"outside {entry['name']} range: {entry}")
                if line["irInstructions"] <= 0 and line["machineInstructions"] <= 0:
                    return None, (f"optimizationInfo O{opt_level}: empty line entry "
                                  f"in {entry['name']}: {line}")
            by_name[entry["name"]] = entry
        return by_name, None

    optimized, err = collect(2)
    if err:
        return err
    for name in ("opt_info_helper", "main"):
        if name not in optimized:
            return f"optimizationInfo O2: missing function {name}: {sorted(optimized)}"

    main_entry = optimized["main"]
    if main_entry["eliminated"]:
        return f"optimizationInfo O2: main was eliminated: {main_entry}"
    if main_entry["machineInstructions"] <= 0 and main_entry["irInstructions"] <= 0:
        return f"optimizationInfo O2: main has no instructions: {main_entry}"
    if main_entry["startLine"] != 5:
        return f"optimizationInfo O2: wrong startLine for main: {main_entry}"

    # Inlining is a heuristic: accept either outcome, but the two must agree with
    # each other - an eliminated body has to have landed somewhere.
    helper = optimized["opt_info_helper"]
    if helper["eliminated"] and helper["inlinedInto"] < 1:
        return f"optimizationInfo O2: helper eliminated but not inlined anywhere: {helper}"
    if not helper["eliminated"]:
        if helper["machineInstructions"] <= 0 and helper["irInstructions"] <= 0:
            return f"optimizationInfo O2: helper survived with no instructions: {helper}"

    unoptimized, err = collect(0)
    if err:
        return err
    helper0 = unoptimized.get("opt_info_helper")
    if helper0 is None or helper0["eliminated"]:
        return f"optimizationInfo O0: helper should survive: {helper0}"
    if helper0["irInstructions"] <= 0:
        return f"optimizationInfo O0: helper has no IR instructions: {helper0}"

    # The body line must be attributed at -O0, where nothing has moved yet.
    if not any(line["srcLine"] == 2 for line in helper0["lines"]):
        return f"optimizationInfo O0: helper body line 2 not attributed: {helper0['lines']}"
    # An eliminated body still has to account for its code somewhere.
    if helper["eliminated"] and helper["lines"]:
        if not any(line["inlined"] for line in helper["lines"]):
            return (f"optimizationInfo O2: eliminated helper has lines but none marked "
                    f"inlined: {helper['lines']}")

    bad = client.request("cflat/optimizationInfo", {"uri": uri, "optLevel": 7}, timeout=30.0)
    if "error" not in bad:
        return f"optimizationInfo: optLevel 7 should be rejected: {bad}"
    return _check_optimization_tiers(client)


def _check_optimization_tiers(client: LspClient) -> str | None:
    """Tier 2 costs / instantiations and Tier 3 remarks, on a file that imports core.

    The import matters: cflat emits one DIFile for the whole module, so core-library
    code carries this file's name with its own line numbers. Everything reported must
    still land inside a function of THIS file.
    """
    source = (
        'import "list.cb";\n'
        "\n"
        "extern int main(int argc, char** argv) {\n"
        "    list<int> nums = default;\n"
        "    for (int i = 0; i < argc; i = i + 1) { nums.add(i * 2); }\n"
        "    int total = 0;\n"
        "    for (int i = 0; i < nums.count(); i = i + 1) { total = total + nums.get(i); }\n"
        "    return total;\n"
        "}\n"
    )
    uri = _uri_for("optimization_tiers")
    _open_doc(client, uri, source)
    diagnostics = wait_diagnostics_for(client, uri)
    if diagnostics:
        return f"optimizationInfo tiers: unexpected diagnostics: {diagnostics}"

    response = client.request("cflat/optimizationInfo",
                              {"uri": uri, "optLevel": 2}, timeout=180.0)
    if "error" in response:
        return f"optimizationInfo tiers: unexpected error: {response}"
    result = response.get("result")
    for field in ("costs", "remarks", "instantiations"):
        if not isinstance(result.get(field), list):
            return f"optimizationInfo tiers: {field} is not a list: {result.keys()}"
    if not isinstance(result.get("remarksTruncated"), bool):
        return f"optimizationInfo tiers: remarksTruncated not a bool: {result}"

    ranges = [(f["startLine"], f["endLine"]) for f in result["functions"]]
    if not ranges:
        return "optimizationInfo tiers: no functions returned"

    def in_range(line: int) -> bool:
        return any(start <= line <= end for start, end in ranges)

    # The regression this guards: core-library lines leaking in as this file's lines.
    for cost in result["costs"]:
        for field in ("kind", "detail"):
            if not isinstance(cost.get(field), str):
                return f"optimizationInfo tiers: cost {field} not a string: {cost}"
        for field in ("srcLine", "bytes", "count"):
            if not isinstance(cost.get(field), int):
                return f"optimizationInfo tiers: cost {field} not an int: {cost}"
        if not in_range(cost["srcLine"]):
            return f"optimizationInfo tiers: cost outside every function: {cost}"

    user_functions = {f["name"] for f in result["functions"]}
    for remark in result["remarks"]:
        for field in ("pass", "name", "kind", "message", "function", "file"):
            if not isinstance(remark.get(field), str):
                return f"optimizationInfo tiers: remark {field} not a string: {remark}"
        if remark["kind"] not in ("passed", "missed", "analysis"):
            return f"optimizationInfo tiers: bad remark kind: {remark}"
        if not isinstance(remark.get("args"), dict):
            return f"optimizationInfo tiers: remark args not an object: {remark}"
        if not in_range(remark["srcLine"]):
            return f"optimizationInfo tiers: remark outside every function: {remark}"
        if remark["function"] not in user_functions:
            return f"optimizationInfo tiers: remark on a non-user function: {remark}"

    for group in result["instantiations"]:
        if not isinstance(group.get("base"), str) or not group["base"]:
            return f"optimizationInfo tiers: bad instantiation base: {group}"
        for field in ("count", "bytes"):
            if not isinstance(group.get(field), int):
                return f"optimizationInfo tiers: instantiation {field} not an int: {group}"
        if not isinstance(group.get("symbols"), list):
            return f"optimizationInfo tiers: instantiation symbols not a list: {group}"

    # list<int> is instantiated by this file, so the registry must report it.
    if not any(group["base"] == "list" for group in result["instantiations"]):
        bases = [group["base"] for group in result["instantiations"]]
        return f"optimizationInfo tiers: list instantiation not reported: {bases}"

    # Opting out of remarks must actually skip them, not just hide them.
    without = client.request("cflat/optimizationInfo",
                             {"uri": uri, "optLevel": 2, "remarks": False}, timeout=180.0)
    if "error" in without:
        return f"optimizationInfo tiers: remarks:false errored: {without}"
    if without["result"]["remarks"]:
        return f"optimizationInfo tiers: remarks:false still returned remarks"
    if not without["result"]["functions"]:
        return "optimizationInfo tiers: remarks:false lost the Tier 1 data"

    bad = client.request("cflat/optimizationInfo",
                         {"uri": uri, "remarks": "yes"}, timeout=30.0)
    if "error" not in bad:
        return f"optimizationInfo tiers: non-boolean remarks should be rejected: {bad}"
    return None


def test_view_assembly(client: LspClient) -> str | None:
    """Check inline-stack attribution and selectable optimization levels."""
    source = (
        "int ir_view_helper(int value) {\n"
        "    return value * 3 + 1;\n"
        "}\n"
        "\n"
        "extern int main(int argc, char** argv) {\n"
        "    return ir_view_helper(argc);\n"
        "}\n"
    )
    uri = _uri_for("view_assembly")
    _open_doc(client, uri, source)
    diagnostics = wait_diagnostics_for(client, uri)
    if diagnostics:
        return f"viewAssembly: unexpected diagnostics: {diagnostics}"

    helper_line = 2
    call_line = 6
    for kind in ("ir", "asm"):
        response = client.request("cflat/viewAssembly", {
            "uri": uri, "kind": kind, "optLevel": 2
        })
        if "error" in response:
            return f"viewAssembly {kind} O2: unexpected error: {response}"
        result = response.get("result")
        if not isinstance(result, dict) or not result.get("text") or not result.get("mappings"):
            return f"viewAssembly {kind} O2: missing text or mappings: {response}"
        inline = next((mapping for mapping in result["mappings"]
                       if isinstance(mapping.get("stack"), list)
                       and len(mapping["stack"]) >= 2), None)
        if inline is None:
            return f"viewAssembly {kind} O2: no multi-frame mapping: {result['mappings']}"
        stack = inline["stack"]
        if stack[0].get("line") != helper_line or not stack[0].get("root"):
            return f"viewAssembly {kind} O2: wrong innermost frame: {stack}"
        if not any(frame.get("line") == call_line and frame.get("root") for frame in stack[1:]):
            return f"viewAssembly {kind} O2: missing call-site frame: {stack}"
        if kind == "ir":
            lines = result["text"].splitlines()
            start = inline.get("start", 0)
            if start <= 0 or start > len(lines) or not re.search(r"\b(ret|add|mul|load|store|call)\b", lines[start - 1]):
                return f"viewAssembly IR O2: mapping is not aligned to an instruction at {start}: {lines[max(0, start - 2):start + 1]}"

    no_inline = client.request("cflat/viewAssembly", {
        "uri": uri, "kind": "ir", "optLevel": 0
    })
    if "error" in no_inline:
        return f"viewAssembly O0: unexpected error: {no_inline}"
    o0 = no_inline.get("result", {})
    if any(isinstance(mapping.get("stack"), list) and len(mapping["stack"]) >= 2
           for mapping in o0.get("mappings", [])):
        return f"viewAssembly O0: helper unexpectedly has an inline stack: {o0}"

    legacy = client.request("cflat/viewAssembly", {
        "uri": uri, "kind": "ir", "optimized": True
    })
    legacy_result = legacy.get("result")
    if "error" in legacy or not isinstance(legacy_result, dict) or not legacy_result.get("text"):
        return f"viewAssembly legacy optimized request failed: {legacy}"
    return None


def test_view_assembly_incremental(client: LspClient) -> str | None:
    """Exercise the O2 incremental view and root-scoped assembly mappings."""
    source = (
        "int incremental_view_helper(int value) {\n"
        "    return value * 3 + 1;\n"
        "}\n\n"
        "extern int main(int argc, char** argv) {\n"
        "    return incremental_view_helper(argc);\n"
        "}\n"
    )
    edited = source.replace("return value * 3 + 1;", "return value * 4 + 1;")
    uri = _uri_for("view_assembly_incremental")
    _open_doc(client, uri, source)
    diagnostics = wait_diagnostics_for(client, uri)
    if diagnostics:
        return f"viewAssembly incremental: unexpected diagnostics: {diagnostics}"
    first = client.request("cflat/viewAssembly", {
        "uri": uri, "kind": "ir", "optLevel": 2, "wholeModule": True
    })
    if "error" in first:
        return f"viewAssembly incremental: initial request failed: {first}"
    first_result = first.get("result", {})
    client.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 2},
        "contentChanges": [{"text": edited}],
    })
    client.notify("textDocument/didSave", {
        "textDocument": {"uri": uri}, "text": edited
    })
    diagnostics = wait_diagnostics_for(client, uri)
    if diagnostics:
        return f"viewAssembly incremental: edited diagnostics: {diagnostics}"
    second = client.request("cflat/viewAssembly", {
        "uri": uri, "kind": "ir", "optLevel": 2, "wholeModule": True
    })
    if "error" in second:
        return f"viewAssembly incremental: edited request failed: {second}"
    result = second.get("result", {})
    if result.get("text") == first_result.get("text"):
        return "viewAssembly incremental: edited text did not change"
    if result.get("timings", {}).get("incremental") is not True:
        return f"viewAssembly incremental: timings did not report incremental: {result}"
    asm = client.request("cflat/viewAssembly", {
        "uri": uri, "kind": "asm", "optLevel": 2, "wholeModule": False
    })
    if "error" in asm:
        return f"viewAssembly incremental asm: request failed: {asm}"
    asm_result = asm.get("result", {})
    if not asm_result.get("mappings"):
        return f"viewAssembly incremental asm: mappings are empty: {asm_result}"
    return None



def _status(name: str, err: str | None) -> tuple:
    """Turn a run_fixture()/test_*() error result into a (name, status, message) triple."""
    return (name, "fail" if err else "pass", err)


def _parse_shard_count_and_args(extra_args: list) -> tuple[int, list]:
    """Pull --lsp-pool-size N out of extra_args to use as the SHARD COUNT (default 4);
    forward --lsp-pool-size 1 to each shard instead - a shard only ever has one analysis
    in flight, so a bigger per-shard pool just allocates idle LLVMBackends for nothing."""
    shard_count = 4
    remaining: list = []
    i = 0
    while i < len(extra_args):
        if extra_args[i] == "--lsp-pool-size" and i + 1 < len(extra_args):
            try:
                shard_count = int(extra_args[i + 1])
            except ValueError:
                pass
            i += 2
            continue
        remaining.append(extra_args[i])
        i += 1
    shard_args = remaining + ["--lsp-pool-size", "1"]
    return shard_count, shard_args


# Sharding, not pipelining: LspServer.cpp keeps a SINGLE currentIndex_ (shared_ptr,
# replaced wholesale at the end of every analysis - see LspServer.cpp GetCurrentIndex),
# so hover/definition/completion always read the MOST RECENTLY analyzed document.
# Interleaving two fixtures' didOpen/hover sequences on one server would clobber that
# slot and make results nondeterministic. Each shard therefore owns its own server
# process and keeps its own fixtures strictly serial - only the shards run concurrently.
def _run_fixture_shard(exe: str, shard_args: list, fixture_items: list) -> tuple:
    """fixture_items: list of (original_index, fixture_path). Returns (results, stderr)."""
    client = LspClient(exe, shard_args)
    results = []
    try:
        initialize(client)
        for idx, fixture_path in fixture_items:
            name = fixture_path.name
            directives, _ = parse_fixture(fixture_path.read_text(encoding="utf-8"))
            req = directives.get("platform")
            if req and req != HOST_PLATFORM:
                results.append((idx, name, "skip", f"requires {req}, host is {HOST_PLATFORM}"))
                continue
            try:
                err = run_fixture(client, fixture_path)
            except TimeoutError as e:
                err = f"TIMEOUT: {e}"
            except Exception as e:
                err = f"EXCEPTION: {e}"
            results.append((idx,) + _status(name, err))
    finally:
        try:
            client.request("shutdown")
            client.notify("exit")
        except Exception:
            pass
        stderr = client.close()
    return results, stderr


def _run_scenario_shard(exe: str, shard_args: list) -> tuple:
    """Runs the hardcoded scenario tests serially on one client. Returns (results, stderr)."""
    client = LspClient(exe, shard_args)
    results = []
    try:
        initialize(client)
        scenarios = [
            ("diagnostic lifecycle", test_diagnostic_lifecycle),
            ("server resilience", test_server_resilience),
            ("def: non-primitives (namespace/struct/local var)", test_def_non_primitives),
            ("def: nested struct/class", test_def_nested_struct),
            ("viewAssembly: inline attribution", test_view_assembly),
            ("viewAssembly: incremental O2 and asm mappings", test_view_assembly_incremental),
            ("optimizationInfo: tier 1 facts", test_optimization_info),
        ]
        for name, fn in scenarios:
            try:
                err = fn(client)
            except Exception as e:
                err = f"EXCEPTION: {e}"
            results.append(_status(name, err))
    finally:
        try:
            client.request("shutdown")
            client.notify("exit")
        except Exception:
            pass
        stderr = client.close()
    return results, stderr


def _run_negative_hover(exe: str) -> tuple:
    try:
        err = test_negative_hover_before_initialize(exe)
    except Exception as e:
        err = f"EXCEPTION: {e}"
    return _status("negative: hover before initialize", err)


def _run_reanalysis(exe: str) -> tuple:
    name = "reanalysis: B-after-A state isolation (L1)"
    # The L1 repro pins Test/test_threadpool.cb as "file A", which uses os.windows.*
    # and so is not clean standalone off Windows (it is in test.sh's skip list). The
    # regression stays covered by the Windows test_lsp.bat run.
    if HOST_PLATFORM != "windows":
        return (name, "skip", "repro files are Windows-only")
    try:
        err = test_reanalysis_state_isolation(exe)
    except Exception as e:
        err = f"EXCEPTION: {e}"
    return _status(name, err)


def run_all(exe: str, extra_args: list) -> bool:
    if not FIXTURE_DIR.exists():
        print(f"error: fixture directory not found: {FIXTURE_DIR}", file=sys.stderr)
        return False

    fixtures = sorted(FIXTURE_DIR.glob("*.cb"))
    print(f"Server:   {exe}")
    print(f"Fixtures: {FIXTURE_DIR} ({len(fixtures)} files)\n")

    shard_count, shard_args = _parse_shard_count_and_args(extra_args)
    shard_count = max(1, min(shard_count, len(fixtures))) if fixtures else 1

    fixtures_indexed = list(enumerate(fixtures))
    shards = [fixtures_indexed[i::shard_count] for i in range(shard_count)]

    with concurrent.futures.ThreadPoolExecutor(max_workers=shard_count + 3) as pool:
        fixture_futures = [pool.submit(_run_fixture_shard, exe, shard_args, shard) for shard in shards]
        scenario_future = pool.submit(_run_scenario_shard, exe, shard_args)
        negative_future = pool.submit(_run_negative_hover, exe)
        reanalysis_future = pool.submit(_run_reanalysis, exe)

        stderrs: list = []
        fixture_results: list = []
        for f in fixture_futures:
            results, stderr = f.result()
            fixture_results.extend(results)
            if stderr.strip():
                stderrs.append(stderr.strip())
        fixture_results.sort(key=lambda t: t[0])

        scenario_results, scenario_stderr = scenario_future.result()
        if scenario_stderr.strip():
            stderrs.append(scenario_stderr.strip())

        negative_result = negative_future.result()
        reanalysis_result = reanalysis_future.result()

    # All threads have joined - print results in the fixed, deterministic order below.
    passed = 0
    failed = 0
    skipped = 0

    def emit(name: str, status: str, message: str | None):
        nonlocal passed, failed, skipped
        if status == "skip":
            print(f"  SKIP  {name} ({message})")
            skipped += 1
        elif status == "pass":
            print(f"  PASS  {name}")
            passed += 1
        else:
            print(f"  FAIL  {name}")
            print(f"        {message}")
            failed += 1

    for _idx, name, status, message in fixture_results:
        emit(name, status, message)

    for name, status, message in scenario_results:
        emit(name, status, message)

    emit(*negative_result)
    emit(*reanalysis_result)

    print(f"\n{passed} passed, {failed} failed")
    if stderrs:
        print("\n--- server stderr ---")
        print("\n\n".join(stderrs))

    if skipped:
        print(f"({skipped} skipped: platform-specific)")
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
