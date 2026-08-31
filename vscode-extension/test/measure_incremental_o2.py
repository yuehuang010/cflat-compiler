#!/usr/bin/env python3
"""Measure and compare incremental and fresh O2 LSP IR views."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Optional
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from lsp_client import LspClient, find_exe, initialize, wait_diagnostics_for

VIEW = {"kind": "ir", "optLevel": 2, "wholeModule": True}
ERROR = 1
HIT_RE = re.compile(
    r"\[view-incremental\] hit reopt=(\d+)/(\d+) seeds=(\d+)(?: depth=(\S+))?")
MISS_RE = re.compile(r"\[view-incremental\] miss reason=([^\s]+)")
DEFINE_RE = re.compile(r"^define\b")
NAME_RE = re.compile(r"@([^\s(]+)\(")


@dataclass
class Scenario:
    name: str
    edit: Callable[[str], str]
    executable: bool
    target: Optional[str] = None
    edit2: Optional[Callable[[str], str]] = None


@dataclass
class View:
    text: str
    timings: dict
    wall_ms: int


@dataclass
class IncLine:
    kind: str
    reason: str = ""
    reopt: int = 0
    total: int = 0
    seeds: int = 0
    depth: str = "-"


@dataclass
class BlockSet:
    preamble: str
    functions: dict[str, str]
    normalized_preamble: str
    normalized_functions: dict[str, str]


@dataclass
class Comparison:
    identical: int
    renumber: int
    semantic: int
    inc_functions: int
    full_functions: int
    details: list[str] = field(default_factory=list)


@dataclass
class Result:
    scenario: str
    file_name: str
    line: IncLine
    inc: Optional[View]
    full: Optional[View]
    comparison: Optional[Comparison]
    verdict: str
    problems: list[str] = field(default_factory=list)


def uri_for(path: Path) -> str:
    value = str(path.resolve()).replace("\\", "/")
    return "file:///" + quote(value, safe="/")


def has_errors(diagnostics: list) -> bool:
    return any(item.get("severity", ERROR) == ERROR for item in diagnostics)


def diag_text(diagnostics: list) -> str:
    return "; ".join(item.get("message", "") for item in diagnostics[:4]) or "unknown diagnostic"


def start_client(exe: str, import_dir: Optional[Path], incremental: bool,
                 depth: Optional[int]) -> LspClient:
    args = ["--lsp-pool-size", "1"]
    if import_dir is not None:
        args += ["--import-dir", str(import_dir)]
    old = os.environ.get("CFLAT_VIEW_INC_DEPTH")
    old_disable = os.environ.get("CFLAT_VIEW_NO_INCREMENTAL")
    try:
        if incremental and depth is not None:
            os.environ["CFLAT_VIEW_INC_DEPTH"] = str(depth)
        elif not incremental:
            os.environ.pop("CFLAT_VIEW_INC_DEPTH", None)
        if incremental:
            os.environ.pop("CFLAT_VIEW_NO_INCREMENTAL", None)
        else:
            os.environ["CFLAT_VIEW_NO_INCREMENTAL"] = "1"
        client = LspClient(exe, args)
    finally:
        if old_disable is None:
            os.environ.pop("CFLAT_VIEW_NO_INCREMENTAL", None)
        else:
            os.environ["CFLAT_VIEW_NO_INCREMENTAL"] = old_disable
        if old is None:
            os.environ.pop("CFLAT_VIEW_INC_DEPTH", None)
        else:
            os.environ["CFLAT_VIEW_INC_DEPTH"] = old
    initialize(client)
    return client


def current_stderr(client: LspClient) -> str:
    return b"".join(client._stderr_buf).decode("utf-8", errors="replace")


def close_client(client: LspClient) -> str:
    stderr = client.close()
    deadline = time.monotonic() + 0.5

    while time.monotonic() < deadline:
        time.sleep(0.02)
        latest = b"".join(client._stderr_buf).decode("utf-8", errors="replace")
        if latest != stderr:
            stderr = latest
    return stderr


def open_document(client: LspClient, uri: str, text: str) -> list:
    client.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "cflat", "version": 1, "text": text
    }})
    return wait_diagnostics_for(client, uri, timeout=240.0)


def request_view(client: LspClient, params: dict) -> View:
    started = time.monotonic()
    response = client.request("cflat/viewAssembly", params, timeout=300.0)
    wall_ms = int((time.monotonic() - started) * 1000)
    if "error" in response:
        raise RuntimeError(str(response["error"]))
    result = response.get("result")
    if not isinstance(result, dict) or not isinstance(result.get("text"), str):
        raise RuntimeError("view response has no text")
    timings = result.get("timings")
    if not isinstance(timings, dict):
        raise RuntimeError("view response has no timings object")
    return View(result["text"], timings, wall_ms)


def parse_inc_line(stderr: str) -> Optional[IncLine]:
    events = [(m.start(), "hit", m) for m in HIT_RE.finditer(stderr)]
    events += [(m.start(), "miss", m) for m in MISS_RE.finditer(stderr)]
    if not events:
        return None
    _, kind, match = max(events, key=lambda item: item[0])
    if kind == "hit":
        return IncLine("hit", reopt=int(match.group(1)), total=int(match.group(2)),
                       seeds=int(match.group(3)), depth=match.group(4) or "-")
    return IncLine("miss", reason=match.group(1))


NUMBERED_ID_RE = re.compile(r"(@|#|!|%)(\d+)")
# LLVM renames struct types parsed into a context that already has the name
# (%T -> %T.1). Types only - @-name suffixes must stay visible (dup detection).
TYPE_SUFFIX_RE = re.compile(r"(%[A-Za-z_][A-Za-z0-9_$]*(?:\.[A-Za-z_][A-Za-z0-9_$]*)*)\.\d+\b")


def canonical_renumber(text: str) -> str:
    # Map each distinct numbered id (per sigil class) to its first-occurrence
    # index, so consistent renumbering compares equal but swaps do not.
    counters: dict[str, dict[str, int]] = {}

    def sub(match: re.Match) -> str:
        sigil, number = match.group(1), match.group(2)
        table = counters.setdefault(sigil, {})
        if number not in table:
            table[number] = len(table)
        return "%s{%d}" % (sigil, table[number])

    return NUMBERED_ID_RE.sub(sub, TYPE_SUFFIX_RE.sub(r"\1", text))


def split_blocks(text: str) -> BlockSet:
    lines = text.splitlines(keepends=True)
    preamble: list[str] = []
    functions: dict[str, str] = {}
    index = 0
    while index < len(lines):
        # A "; Function Attrs:" comment belongs to the define that follows it.
        attrs_line = (lines[index].startswith("; Function Attrs:")
                      and index + 1 < len(lines)
                      and DEFINE_RE.match(lines[index + 1]))
        if not attrs_line and not DEFINE_RE.match(lines[index]):
            if not lines[index].startswith("; ModuleID"):
                preamble.append(lines[index])
            index += 1
            continue
        start = index
        if attrs_line:
            index += 1
        match = NAME_RE.search(lines[index])
        name = match.group(1) if match else "<unnamed-%d>" % len(functions)
        depth = lines[index].count("{") - lines[index].count("}")
        index += 1
        while index < len(lines) and depth > 0:
            depth += lines[index].count("{") - lines[index].count("}")
            index += 1
        functions[name] = "".join(lines[start:index])
    raw_preamble = "".join(preamble)
    return BlockSet(raw_preamble, functions, canonical_renumber(raw_preamble),
                    {name: canonical_renumber(body)
                     for name, body in functions.items()})


def first_difference(left: str, right: str) -> tuple[str, str]:
    a, b = left.splitlines(), right.splitlines()
    for index in range(max(len(a), len(b))):
        la = a[index] if index < len(a) else "<missing>"
        lb = b[index] if index < len(b) else "<missing>"
        if la != lb:
            return la, lb
    return "<different>", "<different>"


def compare_views(inc_text: str, full_text: str) -> Comparison:
    inc, full = split_blocks(inc_text), split_blocks(full_text)
    identical = renumber = semantic = 0
    details: list[str] = []
    names = list(dict.fromkeys(list(inc.functions) + list(full.functions)))
    for name in names:
        inc_raw, full_raw = inc.functions.get(name), full.functions.get(name)
        inc_norm, full_norm = inc.normalized_functions.get(name), full.normalized_functions.get(name)
        if inc_raw is None or full_raw is None:
            semantic += 1
            details.append("%s: %s vs %s" % (
                name, "missing" if inc_raw is None else "present",
                "missing" if full_raw is None else "present"))
        elif inc_raw == full_raw:
            identical += 1
        elif inc_norm == full_norm:
            renumber += 1
        else:
            semantic += 1
            left, right = first_difference(inc_norm, full_norm)
            details.append("%s: inc=%s | full=%s" % (name, left, right))
    if inc.preamble == full.preamble:
        identical += 1
    elif inc.normalized_preamble == full.normalized_preamble:
        renumber += 1
    else:
        # Preamble line order (type/global/attr declarations) is not part of
        # the bar - compare as multisets of canonically-renumbered lines.
        inc_set = sorted(canonical_renumber(line)
                         for line in inc.preamble.splitlines() if line.strip())
        full_set = sorted(canonical_renumber(line)
                          for line in full.preamble.splitlines() if line.strip())
        if inc_set == full_set:
            renumber += 1
        else:
            semantic += 1
            missing = [line for line in full_set if line not in inc_set]
            extra = [line for line in inc_set if line not in full_set]
            details.append("<preamble>: %d lines only-in-full (e.g. %s) | "
                           "%d only-in-inc (e.g. %s)" % (
                               len(missing), missing[0] if missing else "-",
                               len(extra), extra[0] if extra else "-"))
    return Comparison(identical, renumber, semantic, len(inc.functions),
                      len(full.functions), details)



def replace_once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise ValueError("expected one occurrence of %r" % old)
    return source.replace(old, new)


def demo_scenarios() -> list[Scenario]:
    return [
        Scenario("comment-only", lambda s: s + "// x\n", False),
        Scenario("body-const", lambda s: replace_once(
            s, "int total = top(4)", "int total = top(5)"), True, "main"),
        Scenario("body-new-string", lambda s: replace_once(
            s, 'string text = "stable literal";\n    return (int)text.length();',
            'string text = "stable literal";\n'
            '    string extra = "new incremental literal";\n'
            '    return (int)text.length() + (int)extra.length();'),
            True, "stringScore"),
        Scenario("add-func", lambda s: replace_once(
            s, "extern int main()",
            "int addedHelper(int value) { return value + 17; }\n\nextern int main()"
        ).replace("sideHelper(3);", "sideHelper(3) + addedHelper(2);"),
            True, "main"),
        Scenario("remove-func", lambda s: replace_once(
            replace_once(s, "int sideHelper(int value)\n{\n"
                           "    return value + 11;\n}\n\n", ""),
            " + sideHelper(3)", ""),
            True, "main"),
        Scenario("edit-global", lambda s: replace_once(
            s, "int gScale = 3;", "int gScale = 9;"), True),
        # Second consecutive edit: snapshot and analysis then share the same
        # module composition (the first edit mixes bitcode-cache vs re-analysis).
        Scenario("second-edit", lambda s: replace_once(
            s, "int total = top(4)", "int total = top(5)"), True, "main",
            edit2=lambda s: replace_once(
                s, "int total = top(5)", "int total = top(6)")),
        Scenario("signature", lambda s: replace_once(
            replace_once(s, "int offsetValue(int value)",
                         "int offsetValue(int value, int amount)"),
            "return value + 2;", "return value + amount;"
        ).replace("offsetValue(5)", "offsetValue(5, 9)"),
            True, "offsetValue"),
    ]


def big_scenarios() -> list[Scenario]:
    return [
        Scenario("helper-body", lambda s: replace_once(
            s, "    return value.StructNamedMethod();\n}",
            "    return value.StructNamedMethod() * 3;\n}"),
            True, "testStructNamedMethod"),
        Scenario("body-const", lambda s: replace_once(
            s, 'Test("struct_named_method", testStructNamedMethod(), 7)',
            'Test("struct_named_method", testStructNamedMethod(), 8)'),
            True, "main"),
        Scenario("body-new-string", lambda s: replace_once(
            s, 'printf("%d/%d tests passed.\\n", passed, total);',
            'printf("incremental-new-string\\n");\n'
            '    printf("%d/%d tests passed.\\n", passed, total);'),
            True, "main"),
        Scenario("add-func", lambda s: replace_once(
            replace_once(s, "extern int main()",
                         "int incrementalAddedFunction(int value) { return value + 17; }\n\n"
                         "extern int main()"),
            "int passed = 0;", "int passed = incrementalAddedFunction(0);"),
            True, "main"),
    ]


def run_case(exe: str, path: Path, base: str, scenario: Scenario,
             depth: Optional[int]) -> Result:
    uri = uri_for(path)
    import_dir = ROOT / "Test" / "library" if path.parent.name == "Test" else None
    problems: list[str] = []
    line = IncLine("missing")
    inc_view = full_view = comparison = None
    client: Optional[LspClient] = None
    try:
        edited = scenario.edit(base)
        final = scenario.edit2(edited) if scenario.edit2 else edited

        def apply_edit(active: LspClient, version: int, text: str) -> list:
            active.notify("textDocument/didChange", {
                "textDocument": {"uri": uri, "version": version},
                "contentChanges": [{"text": text}],
            })
            active.notify("textDocument/didSave", {
                "textDocument": {"uri": uri}, "text": text
            })
            return wait_diagnostics_for(active, uri, timeout=300.0)

        client = start_client(exe, import_dir, True, depth)
        try:
            baseline = open_document(client, uri, base)
            if has_errors(baseline):
                problems.append("baseline diagnostics: " + diag_text(baseline))
            first = request_view(client, dict(VIEW, uri=uri))
            if scenario.edit2:
                diagnostics = apply_edit(client, 2, edited)
                if has_errors(diagnostics):
                    problems.append("edited diagnostics: " + diag_text(diagnostics))
                first = request_view(client, dict(VIEW, uri=uri))
            stderr_before = current_stderr(client)
            diagnostics = apply_edit(client, 3 if scenario.edit2 else 2, final)
            if has_errors(diagnostics):
                problems.append("edited diagnostics: " + diag_text(diagnostics))
            inc_view = request_view(client, dict(VIEW, uri=uri))
            stderr_after = current_stderr(client)
            if scenario.executable and inc_view.text == first.text:
                problems.append("post-edit view text did not change")
            if scenario.name == "body-const":
                before = split_blocks(first.text).functions.get(scenario.target or "")
                after = split_blocks(inc_view.text).functions.get(scenario.target or "")
                if before is None or after is None or before == after:
                    problems.append("edited function block did not change: " +
                                    (scenario.target or "<none>"))
        finally:
            stderr = close_client(client)
        if stderr.startswith(stderr_before):
            post_stderr = stderr[len(stderr_before):]
        elif stderr_after.startswith(stderr_before):
            post_stderr = stderr_after[len(stderr_before):]
        else:
            post_stderr = stderr_after
        parsed = parse_inc_line(post_stderr)

        if parsed is None:
            problems.append("no post-edit [view-incremental] stderr line")
        else:
            line = parsed
        # Baseline: SAME edit flow with incremental disabled, so the full rebuild
        # runs on the same re-analysis module composition (a fresh server's first
        # analysis differs: core loads from bitcode cache with different linkage).
        client = start_client(exe, import_dir, False, depth)
        try:
            baseline_diags = open_document(client, uri, base)
            if has_errors(baseline_diags):
                problems.append("baseline-N diagnostics: " + diag_text(baseline_diags))
            request_view(client, dict(VIEW, uri=uri))
            fresh_diags = apply_edit(client, 2, edited)
            if scenario.edit2:
                request_view(client, dict(VIEW, uri=uri))
                fresh_diags = apply_edit(client, 3, final)
            if has_errors(fresh_diags):
                problems.append("fresh diagnostics: " + diag_text(fresh_diags))
            full_view = request_view(client, dict(VIEW, uri=uri))
        finally:
            close_client(client)
        if inc_view is not None and full_view is not None:
            comparison = compare_views(inc_view.text, full_view.text)
    except Exception as error:
        problems.append("exception: " + str(error))
    finally:
        if client is not None:
            close_client(client)
    if problems or comparison is None:
        verdict = "INVALID"
    elif comparison.semantic or (
            comparison.inc_functions != comparison.full_functions):
        verdict = "DIVERGE"
    elif comparison.renumber:
        verdict = "RENUMBER"
    else:
        verdict = "PASS"
    return Result(scenario.name, path.name, line, inc_view, full_view,
                  comparison, verdict, problems)


def print_result(result: Result) -> None:
    line = result.line
    hit = line.kind + ("/" + line.reason if line.kind == "miss"
                       else "(d%s)" % line.depth)
    reopt = "%d/%d" % (line.reopt, line.total) if line.kind == "hit" else "-"
    seeds = str(line.seeds) if line.kind == "hit" else "-"
    c = result.comparison
    counts = "-/-/-" if c is None else "%d/%d/%d" % (
        c.identical, c.renumber, c.semantic)
    funcs = "-/-" if c is None else "%d/%d" % (c.inc_functions, c.full_functions)
    it = result.inc.timings if result.inc else {}
    ft = result.full.timings if result.full else {}
    inc_pair = "%s/%s" % (it.get("analyzeMs", "-"), ft.get("analyzeMs", "-"))
    emit_pair = "%s/%s" % (it.get("emitMs", "-"), ft.get("emitMs", "-"))
    wall_pair = "%s/%s" % (
        result.inc.wall_ms if result.inc else "-",
        result.full.wall_ms if result.full else "-")
    print("%-18s %-14s %-15s %-9s %-5s %-11s %-11s %-11s %-11s %s %s" % (
        result.scenario, result.file_name, hit, reopt, seeds, inc_pair,
        emit_pair, wall_pair, counts, funcs, result.verdict))
    for problem in result.problems:
        print("  INVALID %s/%s: %s" % (
            result.file_name, result.scenario, problem))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--depth", type=str, default=None,
                        help="set CFLAT_VIEW_INC_DEPTH for Server I")
    args = parser.parse_args()
    exe = find_exe()
    if not exe:
        print("cflat executable not found", file=sys.stderr)
        return 1
    init = subprocess.run([exe, "--init-local"], cwd=ROOT,
                          capture_output=True, text=True)
    if init.returncode:
        print("--init-local failed:\n" + init.stdout + init.stderr, file=sys.stderr)
        return 1
    fixture = Path(__file__).resolve().parent / "fixtures" / "demo_view.cb"
    check = subprocess.run([exe, str(fixture), "--check"], cwd=ROOT,
                           capture_output=True, text=True)
    if check.returncode:
        print("demo fixture --check failed:\n" + check.stdout + check.stderr,
              file=sys.stderr)
        return 1
    demo = fixture.read_text(encoding="utf-8")
    big_path = ROOT / "Test" / "test_basic.cb"
    big = big_path.read_text(encoding="utf-8")
    cases = [(fixture, demo, s) for s in demo_scenarios()]
    cases += [(big_path, big, s) for s in big_scenarios()]
    print("scenario           file           hit/reason      reopt     seeds analyze I/F emit I/F wall I/F blocks      funcs   verdict")
    print("-" * 138)
    results = []
    for path, source, scenario in cases:
        result = run_case(exe, path, source, scenario, args.depth)
        results.append(result)
        print_result(result)
    print("\nDetails (semantic blocks):")
    count = omitted = 0
    for result in results:
        if not result.comparison:
            continue
        for detail in result.comparison.details:
            if count < 120:
                print("  %s/%s: %s" % (result.file_name, result.scenario, detail))
                count += 1
            else:
                omitted += 1
    if omitted:
        print("  ... %d additional semantic blocks omitted to keep stdout bounded" % omitted)
    invalid = sum(result.verdict == "INVALID" for result in results)
    diverged = sum(result.verdict == "DIVERGE" for result in results)
    print("\nSummary: %d rows, %d PASS, %d DIVERGE, %d INVALID" % (
        len(results), len(results) - invalid - diverged, diverged, invalid))
    return 1 if invalid else 0


if __name__ == "__main__":
    raise SystemExit(main())
