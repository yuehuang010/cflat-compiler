"""Shared comparison and edit scenarios for incremental O2 view tests.

R1-R3 are a provisional baseline pin, accepted pending the maintainer ruling
recorded in internal/plan/ir-view-incremental.md. R2 is the residual the plan
flags as not-merely-cosmetic.
"""

from __future__ import annotations

import re
from collections import Counter
from dataclasses import dataclass, field
from typing import Callable, Optional


DEFINE_RE = re.compile(r"^define\b")
NAME_RE = re.compile(r"@([^\s(]+)\(")
NUMBERED_ID_RE = re.compile(r"(@|#|!|%)(\d+)")
# LLVM renames struct types parsed into a context that already has the name.
TYPE_SUFFIX_RE = re.compile(
    r"(%[A-Za-z_][A-Za-z0-9_$]*(?:\.[A-Za-z_][A-Za-z0-9_$]*)*)\.\d+\b")
RANGE_ATTR_RE = re.compile(r"range\(i\d+ -?\d+, -?\d+\) ")
INLINE_HISTORY_RE = re.compile(r",? !inline_history !(?:\d+|\{\d+\})")
FUNCTION_ATTRS_RE = re.compile(r"^; Function Attrs:")
# Preamble-side of R2/R3, matched by REFERENCE rather than by content: an
# attribute group is droppable only when no define uses it, and a metadata node
# only when !inline_history is what refers to it.
ATTR_GROUP_DEF_RE = re.compile(r"^attributes #(\d+) = ")
ATTR_REF_RE = re.compile(r"#(\d+)\b")
METADATA_DEF_RE = re.compile(r"^!(\d+) = ")
INLINE_HISTORY_REF_RE = re.compile(r"!inline_history !(\d+)")


@dataclass
class Scenario:
    name: str
    edit: Callable[[str], str]
    executable: bool
    target: Optional[str] = None
    edit2: Optional[Callable[[str], str]] = None


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
    known: int
    semantic: int
    inc_functions: int
    full_functions: int
    details: list[str] = field(default_factory=list)
    unexplained: list[str] = field(default_factory=list)


def canonical_renumber(text: str) -> str:
    """Map each numbered LLVM id to its first-occurrence index per sigil."""
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
        # A Function Attrs comment belongs to the define that follows it.
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


def _without_residuals(line: str) -> str:
    return INLINE_HISTORY_RE.sub("", RANGE_ATTR_RE.sub("", line))


def _known_body_difference(left: str, right: str) -> bool:
    a, b = left.splitlines(), right.splitlines()
    if len(a) != len(b):
        return False
    return all(x == y or _without_residuals(x) == _without_residuals(y)
               for x, y in zip(a, b))


def droppable_preamble_ids(module_text: str) -> tuple[set[str], set[str]]:
    """R2/R3 preamble ids: declare-only attribute groups, inline_history nodes."""
    define_attrs: set[str] = set()
    declare_attrs: set[str] = set()
    for line in module_text.splitlines():
        if line.startswith("define"):
            define_attrs.update(ATTR_REF_RE.findall(line))
        elif line.startswith("declare"):
            declare_attrs.update(ATTR_REF_RE.findall(line))
    return declare_attrs - define_attrs, set(INLINE_HISTORY_REF_RE.findall(module_text))


def _preamble_lines(text: str,
                    drop: Optional[tuple[set[str], set[str]]] = None) -> list[str]:
    # Filter on the RAW line: canonical_renumber is per-line, so it erases the
    # very ids the drop sets are keyed by.
    lines = [line for line in text.splitlines() if line.strip()]
    if drop is not None:
        attr_ids, metadata_ids = drop
        kept = []
        for line in lines:
            if FUNCTION_ATTRS_RE.match(line):
                continue
            group = ATTR_GROUP_DEF_RE.match(line)
            if group and group.group(1) in attr_ids:
                continue
            node = METADATA_DEF_RE.match(line)
            if node and node.group(1) in metadata_ids:
                continue
            kept.append(line)
        lines = kept
    return sorted(canonical_renumber(line) for line in lines)


def compare_views(inc_text: str, full_text: str) -> Comparison:
    inc, full = split_blocks(inc_text), split_blocks(full_text)
    identical = renumber = known = semantic = 0
    details: list[str] = []
    unexplained: list[str] = []
    names = list(dict.fromkeys(list(inc.functions) + list(full.functions)))
    for name in names:
        inc_raw, full_raw = inc.functions.get(name), full.functions.get(name)
        inc_norm = inc.normalized_functions.get(name)
        full_norm = full.normalized_functions.get(name)
        if inc_raw is None or full_raw is None:
            semantic += 1
            detail = "%s: %s vs %s" % (
                name, "missing" if inc_raw is None else "present",
                "missing" if full_raw is None else "present")
            details.append(detail)
            unexplained.append(detail)
        elif inc_raw == full_raw:
            identical += 1
        elif inc_norm == full_norm:
            renumber += 1
        elif _known_body_difference(inc_norm, full_norm):
            known += 1
        else:
            semantic += 1
            left, right = first_difference(inc_norm, full_norm)
            detail = "%s: inc=%s | full=%s" % (name, left, right)
            details.append(detail)
            unexplained.append(detail)

    if inc.preamble == full.preamble:
        identical += 1
    elif inc.normalized_preamble == full.normalized_preamble:
        renumber += 1
    else:
        inc_set = _preamble_lines(inc.preamble)
        full_set = _preamble_lines(full.preamble)
        inc_drop = droppable_preamble_ids(inc_text)
        full_drop = droppable_preamble_ids(full_text)
        if inc_set == full_set:
            renumber += 1
        elif (_preamble_lines(inc.preamble, inc_drop) ==
              _preamble_lines(full.preamble, full_drop)):
            known += 1
        else:
            semantic += 1
            inc_filtered = _preamble_lines(inc.preamble, inc_drop)
            full_filtered = _preamble_lines(full.preamble, full_drop)
            missing = list((Counter(full_filtered) - Counter(inc_filtered)).elements())
            extra = list((Counter(inc_filtered) - Counter(full_filtered)).elements())
            detail = ("<preamble>: %d lines only-in-full (e.g. %s) | "
                      "%d only-in-inc (e.g. %s)" % (
                          len(missing), missing[0] if missing else "-",
                          len(extra), extra[0] if extra else "-"))
            details.append(detail)
            unexplained.append(detail)
    return Comparison(identical, renumber, known, semantic,
                      len(inc.functions), len(full.functions), details,
                      unexplained)


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
