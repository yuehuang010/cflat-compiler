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
from typing import Optional
from urllib.parse import quote

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(ROOT / "Test" / "tools"))
from lsp_client import LspClient, find_exe, initialize, wait_diagnostics_for
from view_compare import Comparison, Scenario, big_scenarios, compare_views, demo_scenarios, split_blocks

VIEW = {"kind": "ir", "optLevel": 2, "wholeModule": True}
ERROR = 1
HIT_RE = re.compile(
    r"\[view-incremental\] hit reopt=(\d+)/(\d+) seeds=(\d+)(?: depth=(\S+))?")
MISS_RE = re.compile(r"\[view-incremental\] miss reason=([^\s]+)")
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
            if path.name == "test_basic.cb":
                if line.kind != "miss" or line.reason != "too-wide":
                    problems.append("pinned expectation: miss/too-wide")
            elif line.kind != "hit":
                problems.append("pinned expectation: hit")
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
    elif comparison.unexplained or (
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
    counts = "-/-/-/-" if c is None else "%d/%d/%d/%d" % (
        c.identical, c.renumber, c.known, c.semantic)
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
    print("scenario           file           hit/reason      reopt     seeds analyze I/F emit I/F wall I/F blocks          funcs   verdict")
    print("-" * 142)
    results = []
    for path, source, scenario in cases:
        result = run_case(exe, path, source, scenario, args.depth)
        results.append(result)
        print_result(result)
    print("\nDetails (unexplained blocks):")
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
    return 1 if invalid or diverged else 0


if __name__ == "__main__":
    raise SystemExit(main())
