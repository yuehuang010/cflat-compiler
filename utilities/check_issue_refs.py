#!/usr/bin/env python3
"""Report references to issue files that no longer exist.

usage: check_issue_refs.py [FILE ...]    default: every .md under internal/issue/

A reference is `[[slug]]`, `internal/issue/<path>.md`, or a bucket path such as `p3/<slug>` /
`hold/<slug>`. A slug resolves when any internal/issue/**/<slug>.md exists. A dangling reference
is usually a blocker or follow-up that landed (its file was deleted on fix) and is still being
cited as open. Exits 1 if any reference dangles.
"""
import pathlib, re, sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
ISSUES = ROOT / "internal" / "issue"
WIKI = re.compile(r"\[\[([a-z0-9][a-z0-9-]*)\]\]")
FULL = re.compile(r"internal/issue/([A-Za-z0-9_./-]+?)\.md")
BUCKET = re.compile(r"(?<![\w/.-])((?:p[1-4]|hold)/(?!(?:p[1-4]|hold)(?![\w-]))[a-z0-9][a-z0-9-]*[a-z0-9])(?![\w/-])")


def main():
    files = [pathlib.Path(f) for f in sys.argv[1:]] or sorted(ISSUES.rglob("*.md"))
    slugs = {p.stem for p in ISSUES.rglob("*.md")}
    dangling = 0
    for f in files:
        for n, line in enumerate(f.read_text(encoding="utf-8").splitlines(), 1):
            refs = [(m, (ISSUES / (m + ".md")).exists()) for m in FULL.findall(line)]
            refs += [(m, m in slugs) for m in WIKI.findall(line)]
            refs += [(m, (ISSUES / (m + ".md")).exists()) for m in BUCKET.findall(line)]
            for ref, ok in refs:
                if not ok:
                    dangling += 1
                    print("%s:%d: %s" % (f.relative_to(ROOT).as_posix(), n, ref))
    if dangling:
        print("%d dangling issue reference(s)" % dangling)
    return 1 if dangling else 0


if __name__ == "__main__":
    sys.exit(main())
