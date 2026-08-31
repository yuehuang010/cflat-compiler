#!/usr/bin/env python3
"""CLI-only equivalence gate for incremental optimized IR views."""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from view_compare import (Comparison, Scenario, big_scenarios, compare_views,
                          demo_scenarios)

HIT_RE = re.compile(
    r"\[view-incremental\] hit reopt=(\d+)/(\d+) seeds=(\d+)(?: depth=(\S+))?")
MISS_RE = re.compile(r"\[view-incremental\] miss reason=([^\s]+)")
ERROR_RE = re.compile(r"\(\d+,\d+\):\s*(?:fatal )?error:", re.IGNORECASE)
KNOWN_MISS_REASONS = {"too-wide", "address-taken", "root-file", "no-snapshot",
                      "env-disabled"}


@dataclass
class IncLine:
    kind: str
    reason: str = ""
    reopt: int = 0
    total: int = 0
    seeds: int = 0
    depth: str = "-"


@dataclass
class Case:
    name: str
    source_path: Path
    base: str
    scenario: Scenario
    import_dir: Path | None = None
    corpus: bool = False


@dataclass
class CaseResult:
    case: Case
    line: IncLine | None = None
    comparison: Comparison | None = None
    baseline_line: IncLine | None = None
    problems: list[str] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return not self.problems


def parse_inc_line(stderr: str) -> IncLine | None:
    events = [(match.start(), "hit", match) for match in HIT_RE.finditer(stderr)]
    events += [(match.start(), "miss", match) for match in MISS_RE.finditer(stderr)]
    if not events:
        return None
    _, kind, match = max(events, key=lambda item: item[0])
    if kind == "hit":
        return IncLine("hit", reopt=int(match.group(1)), total=int(match.group(2)),
                       seeds=int(match.group(3)), depth=match.group(4) or "-")
    return IncLine("miss", reason=match.group(1))


def split_dump(stdout: str) -> list[str]:
    chunks = stdout.split("; ==== ")
    result: list[str] = []
    for index, chunk in enumerate(chunks):
        if index:
            _, separator, body = chunk.partition("\n")
            if not separator:
                body = ""
        else:
            body = chunk
        result.append(body)
    return result


def compiler_error(stdout: str) -> bool:
    return any(ERROR_RE.search(line) for line in stdout.splitlines())


def run_dump(exe: str, paths: list[Path], import_dir: Path | None,
             disabled: bool) -> subprocess.CompletedProcess[str]:
    command = [exe, *(str(path) for path in paths)]
    if import_dir is not None:
        command += ["-i", str(ROOT / "Test"), "-i", str(import_dir)]
    command += ["--symbol-dump-opt", "module"]
    environment = os.environ.copy()
    if disabled:
        environment["CFLAT_VIEW_NO_INCREMENTAL"] = "1"
    return subprocess.run(command, cwd=ROOT, env=environment,
                          capture_output=True, text=True)


def write_case(case: Case, case_root: Path) -> tuple[list[Path], str, str]:
    edited = case.scenario.edit(case.base)
    final = case.scenario.edit2(edited) if case.scenario.edit2 else edited
    sources = [case.base, final]
    if case.scenario.edit2:
        sources = [case.base, edited, final]
    paths: list[Path] = []
    for index, source in enumerate(sources, 1):
        directory = case_root / str(index)
        directory.mkdir(parents=True, exist_ok=True)
        path = directory / "probe.cb"
        path.write_text(source, encoding="utf-8")
        paths.append(path)
    return paths, case.base, final


def check_expectation(case: Case, line: IncLine, result: CaseResult) -> None:
    if case.corpus:
        if line.kind == "miss" and line.reason not in KNOWN_MISS_REASONS:
            result.problems.append("unknown miss reason=" + line.reason)
        return
    if case.source_path.name == "test_basic.cb":
        if line.kind != "miss" or line.reason != "too-wide":
            result.problems.append("pinned expectation miss/too-wide")
        return
    if line.kind != "hit":
        result.problems.append("pinned expectation hit")
        return
    if case.scenario.name == "comment-only" and line.reopt != 0:
        result.problems.append("pinned expectation reopt=0")
    elif case.scenario.name != "comment-only" and line.reopt > 25:
        result.problems.append("pinned expectation reopt<=25")


def run_case(exe: str, case: Case, root: Path) -> CaseResult:
    result = CaseResult(case)
    try:
        case_root = root / case.name
        paths, base, final = write_case(case, case_root)
        incremental = run_dump(exe, paths, case.import_dir, False)
        baseline = run_dump(exe, paths, case.import_dir, True)
        if incremental.returncode != 0:
            result.problems.append("incremental exit=%d" % incremental.returncode)
        if baseline.returncode != 0:
            result.problems.append("baseline exit=%d" % baseline.returncode)
        if compiler_error(incremental.stdout) or compiler_error(baseline.stdout):
            result.problems.append("compiler error line in stdout")
        result.line = parse_inc_line(incremental.stderr)
        result.baseline_line = parse_inc_line(baseline.stderr)
        if result.line is None:
            result.problems.append("missing incremental stderr line")
        else:
            check_expectation(case, result.line, result)
        if result.baseline_line is None or result.baseline_line.kind != "miss" \
                or result.baseline_line.reason != "env-disabled":
            result.problems.append("baseline was not miss/env-disabled")
        inc_blocks = split_dump(incremental.stdout)
        full_blocks = split_dump(baseline.stdout)
        if len(inc_blocks) < len(paths) or len(full_blocks) < len(paths):
            result.problems.append("missing module dump banner")
        else:
            inc_text = inc_blocks[-1]
            full_text = full_blocks[-1]
            result.comparison = compare_views(inc_text, full_text)
            comparison = result.comparison
            if comparison.inc_functions != comparison.full_functions:
                result.problems.append("function sets differ")
            if comparison.unexplained:
                result.problems.append("unexplained: " + comparison.unexplained[0])
            if case.scenario.name != "comment-only" and inc_blocks[0] == inc_text:
                result.problems.append("edit did not change module dump")
    except Exception as error:
        result.problems.append("exception: " + str(error))
    return result


def make_pinned_cases() -> list[Case]:
    fixture = ROOT / "vscode-extension" / "test" / "fixtures" / "demo_view.cb"
    big_path = ROOT / "Test" / "test_basic.cb"
    demo = fixture.read_text(encoding="utf-8")
    cases = [Case(s.name, fixture, demo, s) for s in demo_scenarios()]
    cases.append(Case("too-wide", big_path, big_path.read_text(encoding="utf-8"),
                      next(s for s in big_scenarios() if s.name == "body-const"),
                      ROOT / "Test" / "library"))
    return cases


CORPUS_FILES = [
    "example/tools/huffman.cb",
    "example/sci/tone_detection.cb",
    "example/sci/kepler_orbit.cb",
    "example/shell/tetris.cb",
    "example/shell/echo.cb",
    "example/shell/pwd.cb",
]


def make_corpus_cases(exe: str, root: Path) -> list[Case]:
    cases: list[Case] = []
    for file_name in CORPUS_FILES:
        path = ROOT / file_name
        check = subprocess.run([exe, str(path), "--check"], cwd=ROOT,
                               capture_output=True, text=True)
        if check.returncode:
            raise RuntimeError("corpus file does not compile: %s\n%s" %
                               (file_name, check.stdout + check.stderr))
        base = path.read_text(encoding="utf-8")
        stem = path.stem.replace("-", "_")
        comment = Scenario(stem + "-comment", lambda source: source +
                           ("" if source.endswith("\n") else "\n") +
                           "// gate probe\n", False)
        helper_name = "__gate_probe_helper_" + stem
        helper = Scenario(stem + "-helper", lambda source, name=helper_name:
                          source + ("" if source.endswith("\n") else "\n") +
                          "int %s(int v) { return v + 1; }\n" % name, True)
        cases += [Case(comment.name, path, base, comment, corpus=True),
                  Case(helper.name, path, base, helper, corpus=True)]
    return cases


def format_result(result: CaseResult) -> str:
    line = result.line or IncLine("missing")
    state = line.kind + ("/" + line.reason if line.kind == "miss" else "")
    reopt = ("%d/%d" % (line.reopt, line.total)
             if line.kind == "hit" else "-")
    comparison = result.comparison
    counts = ("-/-/-/-" if comparison is None else
              "%d/%d/%d/%d" % (comparison.identical, comparison.renumber,
                                 comparison.known, comparison.semantic))
    verdict = "PASS" if result.passed else "FAIL"
    suffix = ""
    if result.problems:
        suffix = " (" + "; ".join(result.problems[:2]) + ")"
    return "%s  %-18s %-12s %s  %s %s" % (
        result.case.name, state, "reopt=" + reopt, counts, verdict, suffix)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", action="store_true")
    parser.add_argument("--jobs", type=int, default=None)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("-v", action="store_true", dest="verbose")
    args = parser.parse_args()
    exe = args.exe or (ROOT / "x64" / "Release" / "cflat")
    if not exe.is_absolute():
        exe = (ROOT / exe).resolve()
    if not exe.exists():
        print("cflat executable not found: %s" % exe, file=sys.stderr)
        return 1
    init = subprocess.run([str(exe), "--init-local"], cwd=ROOT,
                          capture_output=True, text=True)
    if init.returncode:
        print("--init-local failed:\n" + init.stdout + init.stderr, file=sys.stderr)
        return 1
    cases = make_pinned_cases()
    if args.corpus:
        cases += make_corpus_cases(str(exe), ROOT / "scratch" / "inc-gate")
    jobs = args.jobs or min(os.cpu_count() or 1, 8)
    if jobs < 1:
        parser.error("--jobs must be positive")
    gate_root = ROOT / "scratch" / "inc-gate"
    gate_root.mkdir(parents=True, exist_ok=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(run_case, str(exe), case, gate_root) for case in cases]
        results = [future.result() for future in futures]
    for result in results:
        print(format_result(result))
        if args.verbose:
            for problem in result.problems:
                print("  %s" % problem)
    passed = sum(result.passed for result in results)
    failed = len(results) - passed
    if args.corpus:
        corpus_hits = sum(result.line is not None and result.line.kind == "hit"
                          for result in results if result.case.corpus)
        if corpus_hits == 0:
            print("corpus: no hits (vacuous)")
            failed += 1
        else:
            print("corpus: %d hit(s)" % corpus_hits)
    print("== incremental-o2: %d passed, %d failed ==" % (passed, failed))
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
