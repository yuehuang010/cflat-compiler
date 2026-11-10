#!/usr/bin/env python3
"""Lint the test legs under Test/ (expect_error legs and Test() legs).

Checks (rules: internal/fix-issue-lessons.md, "On tests"):
  1 arm-count     (fail)  PASS lines per err fixture == expect_error legs taken on this host
  2 substring     (fail)  every expect_error substring lies in a cflat/ diagnostic format string
                          ({} = any text), and is not the en.json rendering of a message whose
                          wording differs from its format string (T110, copied from the console).
                          Placeholders absorb any text, so plain prose can still pass the first
                          half; the second half is the targeted guard.
  3 pinned-text   (fail)  no path separator, drive letter, or '$'-mangled name in a substring
  4 tautology     (fail)  Test("name", lit, lit) with equal literals, unless it is the direct
                          body of an if const arm
  5 llp64-width   (note)  bare long, or sizeof of a non-fixed enum, in a Test() leg
                          outside a Windows-gated arm

Phases (test.sh and test.bat call both):
  --phase static   checks 2-5, sources only; runs before the workers launch
  --phase outputs  check 1, needs the per-fixture logs in --results; runs after the workers finish

Output: "LINT FAIL: <check>: <file>:<line>: <reason>" per failure, "LINT NOTE: ..." for notes.
With --results DIR each failure also writes lint_<id>.result / .log so the suite summary counts
it as one failed test. Exit 0 when no check fails, 1 when one does, 2 on a usage error.

Check 1 reads per-fixture logs: <name>.log (test.sh, cold), <name>.warm.log (test.sh, warm
cache), policy_<name>.log (test.bat sidecar and policy fixtures), and test_err_<N>.log plus
err_group_<N-1>_of_<M>.rsp (test.bat batched groups, split per fixture at the compiler's
'PASS: <file>' / 'FAIL: <file>' line that ends each file's output). Each PASS line is tied to
the leg that produced it (the diagnostic line printed before it); a leg with none FAILS.
"""

import argparse
import bisect
import glob
import json
import os
import platform
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import extract_diagnostics as ediag  # the one diagnostic extractor; its parser is not copied here

PASS_LINE = "PASS: expected error received"
PRINTED_RUN = 12  # check 2: literal run that ties a substring to an en.json rendering
PLACEHOLDER = None  # a template slot that absorbs any text

# Platform macros the err fixtures and Test/*.cb gate on. Anything else makes an arm unevaluable.
def platform_macros(target):
    windows = target == "windows"
    macos = target == "macos"
    linux = target == "linux"
    arm = platform.machine().lower() in ("arm64", "aarch64") and not windows
    return {
        "__WINDOWS__": int(windows),
        "__POSIX__": int(not windows),
        "__LINUX__": int(linux),
        "__MACOS__": int(macos),
        "__DARWIN__": int(macos),
        "__ARM64__": int(arm),
        "__X86__": int(not arm),
    }


def host_target():
    system = platform.system()
    if system == "Windows":
        return "windows"
    if system == "Darwin":
        return "macos"
    return "linux"


def sanitize(text):
    # Result files are read by cmd (set /p, delayed expansion) on Windows: keep them plain.
    return re.sub(r"[%!^&|<>\"\r\n]", "_", text)


# ---------------------------------------------------------------- source scanning

def mask_code(text):
    """Blank comments and replace string/char bodies with 'S'. Same length as text."""
    out = list(text)
    n = len(text)
    i = 0
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, min(j, n)):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            for k in range(i + 1, min(j, n)):
                out[k] = "S"
            i = j + 1 if j < n and text[j] == c else j
        else:
            i += 1
    return "".join(out)


TOKEN_RE = re.compile(r"\"S*\"|'S*'|[A-Za-z_]\w*|\d\w*(?:\.\w+)?|\S")


def tokenize(code):
    toks = []
    for m in TOKEN_RE.finditer(code):
        s = m.group(0)
        if s[0] in "\"'":
            kind = "str"
        elif s[0].isalpha() or s[0] == "_":
            kind = "id"
        elif s[0].isdigit():
            kind = "num"
        else:
            kind = "p"
        toks.append((kind, s, m.start(), m.end()))
    return toks


class LineIndex:
    def __init__(self, text):
        self.starts = [0] + [m.end() for m in re.finditer("\n", text)]

    def of(self, pos):
        return bisect.bisect_right(self.starts, pos)


def match_paren(toks, j):
    depth = 0
    for k in range(j, len(toks)):
        if toks[k][0] == "p":
            if toks[k][1] == "(":
                depth += 1
            elif toks[k][1] == ")":
                depth -= 1
                if depth == 0:
                    return k
    return None


def split_args(toks, open_idx, close_idx):
    """Token index ranges [start, end) of the comma-separated arguments between two parens."""
    spans = []
    start = open_idx + 1
    depth = 0
    for k in range(open_idx + 1, close_idx):
        kind, s = toks[k][0], toks[k][1]
        if kind != "p":
            continue
        if s in "([{":
            depth += 1
        elif s in ")]}":
            depth -= 1
        elif s == "," and depth == 0:
            spans.append((start, k))
            start = k + 1
    if start < close_idx or spans:
        spans.append((start, close_idx))
    return spans


def and3(a, b):
    if a is False or b is False:
        return False
    if a is None or b is None:
        return None
    return True


def or3(a, b):
    if a is True or b is True:
        return True
    if a is None or b is None:
        return None
    return False


def not3(a):
    return None if a is None else (not a)


COND_TOKEN_RE = re.compile(r"\s*(&&|\|\||[!()]|[A-Za-z_]\w*|\d+)")


def eval_cond(text, macros):
    """Value of a small if const condition on this target, or None when it is not evaluable."""
    toks = []
    pos = 0
    text = text.rstrip()
    while pos < len(text):
        m = COND_TOKEN_RE.match(text, pos)
        if not m:
            return None
        toks.append(m.group(1))
        pos = m.end()
    if not toks:
        return None
    idx = [0]

    def peek():
        return toks[idx[0]] if idx[0] < len(toks) else None

    def take():
        t = peek()
        idx[0] += 1
        return t

    def primary():
        t = take()
        if t is None:
            raise ValueError
        if t == "(":
            v = or_expr()
            if take() != ")":
                raise ValueError
            return v
        if t.isdigit():
            return int(t) != 0
        if t == "true" or t == "false":
            return t == "true"
        if t in macros:
            return bool(macros[t])
        raise ValueError

    def unary():
        if peek() == "!":
            take()
            return not unary()
        return primary()

    def and_expr():
        v = unary()
        while peek() == "&&":
            take()
            v = unary() and v
        return v

    def or_expr():
        v = and_expr()
        while peek() == "||":
            take()
            v = and_expr() or v
        return v

    try:
        value = or_expr()
    except ValueError:
        return None
    if idx[0] != len(toks):
        return None
    return bool(value)


def analyze(text, macros):
    """Walk one source file's tokens: expect_error legs, Test() calls and if const arm state.

    A leg's eff is True when every enclosing arm is taken on this target, False when one is not
    taken, None when an enclosing if const cannot be evaluated.
    """
    code = mask_code(text)
    toks = tokenize(code)
    lines = LineIndex(text)
    legs = []
    calls = []
    unknown_arms = []
    stack = []
    depth = 0
    pending = None
    chain = None      # {"kind", "prior"} of the if chain whose arm closed last
    chain_at = -1     # token index that closed that arm
    cont = None       # chain carried into an "else if"
    cont_at = -1
    n = len(toks)

    def eff_now():
        return stack[-1]["eff"] if stack else True

    def gated_now():
        return stack[-1]["gated"] if stack else False

    def push(entry):
        entry["eff"] = and3(eff_now(), entry["own"])
        entry["gated"] = gated_now() or entry.get("cond_gated", False)
        stack.append(entry)

    def pop_arm(entry, at):
        nonlocal chain, chain_at
        chain = {"kind": entry["chain_kind"], "prior": entry["after"]} if entry["chain_kind"] else None
        chain_at = at

    i = 0
    while i < n:
        kind, s, a, _b = toks[i]
        nxt = toks[i + 1] if i + 1 < n else None
        if kind == "id" and s == "if":
            j = i + 1
            is_const = nxt is not None and nxt[0] == "id" and nxt[1] == "const"
            if is_const:
                j += 1
            prev_chain = cont if (cont is not None and cont_at == i - 1) else None
            cont = None
            if j < n and toks[j][1] == "(" and toks[j][0] == "p":
                close = match_paren(toks, j)
                if close is None:
                    i += 1
                    continue
                cond = code[toks[j][3]:toks[close][2]]
                if is_const:
                    prior = prev_chain["prior"] if prev_chain is not None else False
                    value = eval_cond(cond, macros)
                    own = and3(not3(prior), value)
                    arm = {"own": own, "after": or3(prior, value), "chain_kind": "ifconst",
                           "kind": "ifconst", "cond_gated": "__WINDOWS__" in cond}
                    if own is None:
                        unknown_arms.append((lines.of(a), " ".join(cond.split())))
                else:
                    arm = {"own": True, "after": None, "chain_kind": "runtime", "kind": "runtime",
                           "cond_gated": "__WINDOWS__" in cond}
                if close + 1 < n and toks[close + 1][0] == "p" and toks[close + 1][1] == "{":
                    pending = arm
                else:
                    push(dict(arm, depth=depth, stmt=True))
                i = close + 1
                continue
        elif kind == "id" and s == "else":
            prev_ok = chain is not None and chain_at == i - 1
            if nxt is not None and nxt[0] == "id" and nxt[1] == "if" and prev_ok:
                cont = chain
                cont_at = i
                i += 1
                continue
            if prev_ok and chain["kind"] == "ifconst":
                arm = {"own": not3(chain["prior"]), "after": None, "chain_kind": None,
                       "kind": "ifconst", "cond_gated": False}
            else:
                arm = {"own": True, "after": None, "chain_kind": None, "kind": "runtime",
                       "cond_gated": False}
            if nxt is not None and nxt[0] == "p" and nxt[1] == "{":
                pending = arm
            else:
                push(dict(arm, depth=depth, stmt=True))
            i += 1
            continue
        elif kind == "p" and s == "{":
            depth += 1
            if pending is not None:
                entry = dict(pending)
                pending = None
            else:
                entry = {"own": True, "after": None, "chain_kind": None, "kind": "block",
                         "cond_gated": False}
            entry["depth"] = depth
            entry["stmt"] = False
            push(entry)
            i += 1
            continue
        elif kind == "p" and s == "}":
            while stack and stack[-1]["stmt"] and stack[-1]["depth"] == depth:
                pop_arm(stack.pop(), i)
            if stack and not stack[-1]["stmt"]:
                pop_arm(stack.pop(), i)
            depth -= 1
            i += 1
            continue
        elif kind == "p" and s == ";":
            if stack and stack[-1]["stmt"] and stack[-1]["depth"] == depth:
                pop_arm(stack.pop(), i)
            i += 1
            continue
        elif kind == "id" and s == "expect_error" and nxt is not None and nxt[0] == "p" and nxt[1] == "(":
            close = match_paren(toks, i + 1)
            if close is None:
                i += 1
                continue
            after = toks[close + 1] if close + 1 < n else None
            if after is not None and after[1] == "{":
                form = "scoped"
            elif after is not None and after[1] == ";":
                form = "bare"
            else:
                form = "other"
            legs.append({"line": lines.of(a), "form": form, "eff": eff_now(),
                         "args": (toks[i + 1][3], toks[close][2]), "gated": gated_now(),
                         "tok": i, "close": close})
            i = close + 1
            continue
        elif kind == "id" and s == "Test" and nxt is not None and nxt[0] == "p" and nxt[1] == "(":
            prev = toks[i - 1] if i > 0 else None
            declaration = prev is not None and prev[0] == "id" and prev[1] not in ("return", "else")
            close = match_paren(toks, i + 1)
            if close is None or declaration:
                i += 1
                continue
            spans = split_args(toks, i + 1, close)
            qualifier = toks[i - 2][1] if i >= 2 and toks[i - 1][1] == "." else None
            calls.append({"line": lines.of(a), "spans": spans, "toks": toks,
                          "text": text, "inner": stack[-1]["kind"] if stack else "file",
                          "gated": gated_now(), "qualifier": qualifier})
            i = close + 1
            continue
        i += 1
    return {"legs": legs, "calls": calls, "unknown_arms": unknown_arms, "toks": toks,
            "code": code, "lines": lines}


# ---------------------------------------------------------------- literal helpers

NUMBER_RE = re.compile(r"-?\s*(0[xX][0-9A-Fa-f]+|\d+\.?\d*(?:[eE][-+]?\d+)?)[uUlLfF]*$")


def literal_value(expr):
    """('str', decoded) / ('num', text) / ('bool', text) / ('char', text), or None."""
    stripped = expr.strip()
    value = ediag.parse_string_literal_sequence(stripped)
    if value is not None:
        return ("str", value)
    if stripped in ("true", "false"):
        return ("bool", stripped)
    if NUMBER_RE.match(stripped):
        return ("num", "".join(stripped.split()))
    if len(stripped) >= 3 and stripped[0] == "'" and stripped[-1] == "'":
        return ("char", stripped)
    return None


def span_text(text, toks, spans_tok):
    start, end = spans_tok
    if end <= start:
        return ""
    return text[toks[start][2]:toks[end - 1][3]]


# ---------------------------------------------------------------- diagnostic format strings

FORMAT_CALLS = ("std::format", "std::vformat", "FormatSourceTemplate")
MESSAGE_CALLS = ("LogErrorMessage", "LocalizeMessage", "localizeMessage_", "LogErrorContext", "LogError",
                 "compile_error")


def split_top_plus(expression):
    """Operands of a top-level '+' chain. String bodies and nested brackets are skipped."""
    operands = []
    depth = 0
    start = 0
    i = 0
    n = len(expression)
    while i < n:
        c = expression[i]
        if c in "\"'":
            j = i + 1
            while j < n and expression[j] != c:
                j += 2 if expression[j] == "\\" else 1
            i = j + 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "+" and depth == 0:
            operands.append(expression[start:i].strip())
            start = i + 1
        i += 1
    operands.append(expression[start:].strip())
    return operands


def format_literals(text):
    """Template token lists for diagnostic text that the catalog calls do not carry.

    std::format-style calls: the format string, placeholders kept (brace semantics).
    Message calls built by '+': each literal operand is literal text and each other operand is
    a placeholder, so a substring may span a runtime value but not invent literal text.
    """
    code_positions = ediag.scan_code_positions(text)
    code_set = set(code_positions)
    found = []
    for name in FORMAT_CALLS + MESSAGE_CALLS:
        for start in ediag.find_call_starts(text, code_positions, name):
            paren = ediag.next_code_char(text, start + len(name), code_set)
            if paren < 0 or text[paren] != "(":
                continue
            argument_text = ediag.extract_call_arguments(text, paren)
            if argument_text is None:
                continue
            parts = ediag.split_top_level(argument_text)
            if name in FORMAT_CALLS:
                literal = ediag.parse_string_literal_sequence(parts[0]) if parts else None
                if literal is not None:
                    found.append(("template", literal))
                continue
            for part in parts:
                if '"' not in part:
                    continue
                tokens = []
                for operand in split_top_plus(part):
                    literal = ediag.parse_string_literal_sequence(operand)
                    if literal is not None:
                        tokens.extend(literal)
                    else:
                        tokens.append(PLACEHOLDER)
                found.append(("raw", tokens))
    return found


def template_tokens(template):
    out = []
    i = 0
    while i < len(template):
        if template.startswith("{{", i):
            out.append("{")
            i += 2
        elif template.startswith("}}", i):
            out.append("}")
            i += 2
        elif template[i] == "{":
            j = template.find("}", i)
            if j < 0:
                out.append("{")
                i += 1
            else:
                out.append(PLACEHOLDER)
                i = j + 1
        else:
            out.append(template[i])
            i += 1
    return out


class TemplateIndex:
    """Format-string literal runs from cflat/ sources, indexed for substring acceptance."""

    def __init__(self, cflat_dir):
        templates = set()
        problems = []
        # CFlat library code under cflat/core raises its own diagnostics with compile_error("...").
        sources = sorted(glob.glob(os.path.join(cflat_dir, "*.cpp")) + glob.glob(os.path.join(cflat_dir, "*.h"))
                         + glob.glob(os.path.join(cflat_dir, "**", "*.cb"), recursive=True))
        for path in sources:
            if path.endswith(".cb"):
                with open(path, "r", encoding="utf-8", errors="replace") as handle:
                    text = handle.read()
                for kind, value in format_literals(text):
                    templates.add(value if kind == "template" else ("raw", tuple(value)))
                continue
            entries, _legacy = ediag.scan_file(path, problems)
            for entry in entries:
                templates.add(entry["template"])
            # LogErrorContext(ctx, std::format("...", ...)) builds its text outside the catalog
            # calls scan_file reads, so its format string is a fragment source too.
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
            for kind, value in format_literals(text):
                templates.add(value if kind == "template" else ("raw", tuple(value)))
        built = []
        for entry in sorted(templates, key=repr):
            if isinstance(entry, tuple) and len(entry) == 2 and entry[0] == "raw":
                built.append(list(entry[1]))
            else:
                built.append(template_tokens(entry))
        self.templates = built
        self.by_bigram = {}
        self.by_end = {}
        self.by_char = {}
        for index, toks in enumerate(self.templates):
            for k, tok in enumerate(toks):
                if tok is PLACEHOLDER:
                    continue
                self.by_char.setdefault(tok, set()).add(index)
                nxt = toks[k + 1] if k + 1 < len(toks) else None
                if nxt is None or nxt is PLACEHOLDER:
                    self.by_end.setdefault(tok, set()).add(index)
                else:
                    self.by_bigram.setdefault(tok + nxt, set()).add(index)
        self.cache = {}
        self.runs = None

    def literal_holds(self, sub):
        """True when sub lies inside one literal run of some template."""
        if self.runs is None:
            self.runs = []
            for toks in self.templates:
                run = []
                for tok in toks + [PLACEHOLDER]:
                    if tok is PLACEHOLDER:
                        if run:
                            self.runs.append("".join(run))
                        run = []
                    else:
                        run.append(tok)
        return any(sub in run for run in self.runs)

    def accepts(self, sub, need=0):
        """True when sub occurs in one expansion of some template (placeholders = any text).

        The match must start on literal text, so a substring of pure placeholder text is refused.
        need > 0: the match must also hold one literal run of need characters (see _match).
        """
        if (sub, need) in self.cache:
            return self.cache[(sub, need)]
        result = False
        if not sub:
            result = True
        elif len(sub) == 1:
            candidates = self.by_char.get(sub, set())
        else:
            candidates = set()
            for k in range(len(sub) - 1):
                candidates |= self.by_bigram.get(sub[k:k + 2], set())
            for ch in set(sub):
                candidates |= self.by_end.get(ch, set())
        positions = {}
        for k, ch in enumerate(sub):
            positions.setdefault(ch, []).append(k)
        for index in candidates:
            if self._match(sub, self.templates[index], positions, need):
                result = True
                break
        self.cache[(sub, need)] = result
        return result

    @staticmethod
    def _match(sub, toks, positions, need=0):
        """True when sub is a substring of some expansion of toks.

        A window starts on a literal token. When that token follows a placeholder, the
        placeholder may have absorbed sub's head (a runtime value), so the window may start at
        any position of sub holding that character. need > 0 also asks for one contiguous run of
        at least need characters matched by literal text (the printed-text check uses it; a bare
        placeholder match proves little about which text the substring was copied from).
        """
        n = len(sub)
        for j0, tok in enumerate(toks):
            if tok is PLACEHOLDER:
                continue
            if j0 > 0 and toks[j0 - 1] is PLACEHOLDER:
                starts = positions.get(tok, ())
            else:
                starts = (0,) if n and sub[0] == tok else ()
            for k in starts:
                states = {k + 1: (1, 1)}  # position in sub -> (longest literal run, current run)
                if n in states and states[n][0] >= need:
                    return True
                for j in range(j0 + 1, len(toks)):
                    t = toks[j]
                    if t is PLACEHOLDER:
                        spread = {}
                        best = -1
                        for i in range(min(states), n + 1):
                            if i in states:
                                best = max(best, states[i][0])
                            if best >= 0:
                                spread[i] = (best, 0)
                        states = spread
                    else:
                        states = {i + 1: (max(b, c + 1), c + 1)
                                  for i, (b, c) in states.items() if i < n and sub[i] == t}
                    if n in states and states[n][0] >= need:
                        return True
                    if not states:
                        break
        return False


class PrintedText:
    """en.json renderings whose wording differs from their source format string (T110).

    A substring that matches the rendering and not the source template was copied from the
    console, and expect_error (which matches the format string) never sees it.
    """

    def __init__(self, cflat_dir):
        self.pairs = []
        self.cache = {}
        catalog = os.path.join(cflat_dir, "locales", "en.json")
        if not os.path.isfile(catalog):
            return
        with open(catalog, "r", encoding="utf-8") as handle:
            messages = json.load(handle).get("messages", {})
        problems = []
        sources = {}
        for path in sorted(glob.glob(os.path.join(cflat_dir, "*.cpp")) + glob.glob(os.path.join(cflat_dir, "*.h"))):
            entries, _legacy = ediag.scan_file(path, problems)
            for entry in entries:
                sources.setdefault(ediag.normalize_key(entry["template"]), entry["template"])
        for key, printed in sorted(messages.items()):
            source = sources.get(key)
            if source is None or not isinstance(printed, str):
                continue
            if ediag.numbered_template(source) == printed:
                continue
            plain = ediag.source_template(printed)
            self.pairs.append((source, plain, template_tokens(plain)))
        self.by_gram = {}
        for number, (_source, plain, _toks) in enumerate(self.pairs):
            for k in range(len(plain) - PRINTED_RUN + 1):
                self.by_gram.setdefault(plain[k:k + PRINTED_RUN], set()).add(number)

    def copied_from(self, sub, index):
        """The source template whose printed wording sub was copied from, or None: sub carries
        a literal run of a rendering that no source template carries."""
        if sub in self.cache:
            return self.cache[sub]
        self.cache[sub] = None
        need = min(len(sub), PRINTED_RUN)
        # The literal run is a need-long slice of sub that also lies in the rendering's text.
        grams = {sub[k:k + need] for k in range(len(sub) - need + 1)}
        if need == PRINTED_RUN:
            numbers = set()
            for gram in grams:
                numbers |= self.by_gram.get(gram, set())
            pairs = [self.pairs[number] for number in sorted(numbers)]
        else:
            pairs = [pair for pair in self.pairs if any(g in pair[1] for g in grams)]
        positions = None
        for source, _printed_text, printed_toks in pairs:
            if positions is None:
                positions = {}
                for k, ch in enumerate(sub):
                    positions.setdefault(ch, []).append(k)
            if TemplateIndex._match(sub, printed_toks, positions, need):
                if not index.accepts(sub, need):
                    self.cache[sub] = source
                break
        return self.cache[sub]


# ---------------------------------------------------------------- reporting

class Report:
    def __init__(self, results_dir):
        self.results_dir = results_dir
        self.failures = 0
        self.counts = {}

    def fail(self, check, rel, line, reason):
        msg = sanitize("LINT FAIL: %s: %s:%d: %s" % (check, rel, line, reason))
        print(msg)
        self.failures += 1
        self.counts[check] = self.counts.get(check, 0) + 1
        if self.results_dir:
            ident = re.sub(r"[^A-Za-z0-9]+", "_", "%s_%s_%d" % (check, rel, line)).strip("_")
            base = os.path.join(self.results_dir, "lint_" + ident)
            with open(base + ".result", "w", encoding="ascii", errors="backslashreplace") as handle:
                handle.write("FAIL: " + msg + "\n")
            with open(base + ".log", "w", encoding="ascii", errors="backslashreplace") as handle:
                handle.write(msg + "\n")

    def note(self, check, rel, line, reason):
        print(sanitize("LINT NOTE: %s: %s:%d: %s" % (check, rel, line, reason)))


MANGLED_RE = re.compile(r"[A-Za-z0-9_]\$[A-Za-z_]")


def has_path_shape(token):
    """A separator ('/' or backslash) between path segments, the last of which carries a file
    extension ('dir/file.cb', 'dir\\file.cb'). Prose such as 'struct/union/class' or
    'os.windows.fopen/fread' has no extension.
    """
    parts = re.split(r"[\\/]", token)
    if len(parts) < 2 or not parts[-2].strip("'\"("):
        return False
    last = parts[-1].rstrip("')\":,;.")
    return re.search(r"\.[A-Za-z]\w*$", last) is not None


def rel_of(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def test_cb_files(test_dir):
    return sorted(glob.glob(os.path.join(test_dir, "**", "*.cb"), recursive=True))


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return handle.read()


# ---------------------------------------------------------------- static checks 2-5

def static_checks(test_dir, cflat_dir, report, macros):
    index = TemplateIndex(cflat_dir)
    printed = PrintedText(cflat_dir)
    for path in test_cb_files(test_dir):
        rel = rel_of(path)
        text = read_text(path)
        info = analyze(text, macros)
        for leg in info["legs"]:
            spans = leg["args"]
            expr = text[spans[0]:spans[1]]
            value = literal_value(expr)
            if value is None or value[0] != "str":
                report.note("substring", rel, leg["line"], "expect_error argument is not a single string literal")
                continue
            sub = value[1]
            # Check 3 - pinned text. Text a format-string literal itself carries ('set $SDKROOT',
            # '{}\\bin') is not a pin; a slash between spaces is prose (' / '), not a path.
            if not index.literal_holds(sub):
                if any(has_path_shape(tok) for tok in sub.split()):
                    report.fail("pinned-text", rel, leg["line"],
                                "path separator in expect_error substring '%s'" % sub)
                elif re.search(r"(?<![A-Za-z0-9_])[A-Za-z]:(?=[\\/])", sub):
                    report.fail("pinned-text", rel, leg["line"],
                                "drive letter in expect_error substring '%s'" % sub)
                elif MANGLED_RE.search(sub):
                    report.fail("pinned-text", rel, leg["line"],
                                "mangled generic name ('$'-joined) in expect_error substring '%s'" % sub)
            # Check 2 - substring source.
            if not index.accepts(sub):
                report.fail("substring", rel, leg["line"],
                            "expect_error substring '%s' is not in any cflat/ diagnostic format string" % sub)
            else:
                source = printed.copied_from(sub, index)
                if source is not None:
                    report.fail("substring", rel, leg["line"],
                                "expect_error substring '%s' is the printed en.json wording of "
                                "format string '%s'; copy the format string" % (sub, source))
        # Check 4 - tautological Test legs. Only the helper's Test counts: unqualified, or through
        # an alias of test_helper.cb. Exempt shapes: a qualified call whose qualifier a namespace
        # of the same name shadows (test_basic QoLateAliasUse: the leg is name lookup, its
        # arguments are inert), and a literal leg that is the direct body of an if const arm.
        helper_aliases = set(re.findall(r'import\s+"test_helper\.cb"\s+as\s+(\w+)', text))
        helper_aliases -= set(re.findall(r"\bnamespace\s+(\w+)", info["code"]))
        for call in info["calls"]:
            spans = call["spans"]
            if len(spans) != 3:
                continue
            if call["qualifier"] is not None and call["qualifier"] not in helper_aliases:
                continue
            values = [literal_value(span_text(text, call["toks"], sp)) for sp in spans[1:]]
            if any(v is None for v in values):
                continue
            first = span_text(text, call["toks"], spans[1]).strip()
            second = span_text(text, call["toks"], spans[2]).strip()
            if values[0] != values[1]:
                continue
            if call["inner"] == "ifconst":
                continue
            report.fail("tautology", rel, call["line"],
                        "Test leg with equal literals '%s' and '%s' can never fail" % (first, second))
        # Check 5 - LLP64 width (note only). Collected per file below.
        check_llp64(text, info, rel, report)


def check_llp64(text, info, rel, report):
    for call in info["calls"]:
        if call["gated"]:
            continue
        toks = call["toks"]
        for start, end in call["spans"]:
            for k in range(start, end):
                if toks[k][0] != "id":
                    continue
                if toks[k][1] == "long":
                    before = toks[k - 1][1] if k > 0 else ""
                    after = toks[k + 1][1] if k + 1 < len(toks) else ""
                    if before != "long" and after != "long":
                        report.note("llp64-width", rel, call["line"],
                                    "bare 'long' in a Test leg (32-bit on Windows); use long long or gate it")
                        break
            else:
                continue
            break


def enum_widths(test_dir):
    names = set()
    for path in test_cb_files(test_dir):
        for m in re.finditer(r"\benum\s+(\w+)\s*\{", mask_code(read_text(path))):
            names.add(m.group(1))
    return names


def check_enum_width(test_dir, report):
    names = enum_widths(test_dir)
    if not names:
        return
    for path in test_cb_files(test_dir):
        rel = rel_of(path)
        text = read_text(path)
        info = analyze(text, platform_macros(host_target()))
        for call in info["calls"]:
            if call["gated"]:
                continue
            toks = call["toks"]
            for start, end in call["spans"]:
                for k in range(start, end - 2):
                    if toks[k][1] == "sizeof" and toks[k + 1][1] == "(" and toks[k + 2][1] in names:
                        report.note("llp64-width", rel, call["line"],
                                    "sizeof(%s) in a Test leg: enum width is not fixed" % toks[k + 2][1])


# ---------------------------------------------------------------- check 1 (outputs)

def load_flags(sidecar):
    flags = []
    expect_exit = ""
    if os.path.isfile(sidecar):
        with open(sidecar, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                line = line.rstrip("\r\n")
                if line.startswith("flags="):
                    flags.extend(line[len("flags="):].split())
                elif line.startswith("expect_exit="):
                    expect_exit = line[len("expect_exit="):]
    return flags, expect_exit


def target_for_flags(flags, default):
    for index, token in enumerate(flags):
        if token == "-p" and index + 1 < len(flags):
            value = flags[index + 1].lower()
            if value.startswith("win"):
                return "windows"
            if value in ("macos", "mac", "darwin"):
                return "macos"
            if value == "linux":
                return "linux"
            return None
    return default


def log_has_skip(text):
    return any(line.startswith("SKIP:") for line in text.splitlines())


def taken_legs(text, target):
    """Taken legs whose argument is one string literal, each with the line span it covers, or
    None when an enclosing if const cannot be evaluated on this target. Malformed legs (no
    literal, or one that never closes) are not arms and are not counted.

    Span: a scoped leg covers its block; a bare leg covers its own line to the end of the
    innermost enclosing brace scope (the whole rest of the file at file scope)."""
    info = analyze(text, platform_macros(target))
    if any(leg["eff"] is None for leg in info["legs"]):
        return None, info
    toks = info["toks"]
    lines = info["lines"]
    partner = {}
    opens = []
    for k, tok in enumerate(toks):
        if tok[0] == "p" and tok[1] == "{":
            opens.append(k)
        elif tok[0] == "p" and tok[1] == "}" and opens:
            partner[opens.pop()] = k
    pairs = sorted(partner.items())
    end_of_file = lines.of(len(text)) + 1
    legs = []
    for order, leg in enumerate(info["legs"]):
        if leg["eff"] is not True:
            continue
        value = literal_value(text[leg["args"][0]:leg["args"][1]])
        if value is None or value[0] != "str":
            continue
        close = leg["close"]
        if leg["form"] == "scoped" and close + 1 in partner:
            span = (lines.of(toks[close + 1][2]), lines.of(toks[partner[close + 1]][2]))
        else:
            inner = [(o, c) for o, c in pairs if o < leg["tok"] < c]
            end = lines.of(toks[inner[-1][1]][2]) if inner else end_of_file
            span = (leg["line"], end)
        legs.append({"line": leg["line"], "sub": value[1], "form": leg["form"], "span": span,
                     "order": order, "hits": 0})
    return legs, info


DIAG_RE = re.compile(r"^(?P<file>.+?)\((?P<line>\d+),(?P<col>\d+)\): ")


def pass_events(log_text, basename):
    """One entry per PASS line: the line of the diagnostic printed just before it in this
    fixture, or None (parse-error matches and errors in imported files print no such line)."""
    events = []
    last = None
    for raw in log_text.splitlines():
        line = raw.rstrip()
        if line.strip() == PASS_LINE:
            events.append(last)
            last = None
            continue
        match = DIAG_RE.match(line)
        if match:
            name = re.split(r"[\\/]", match.group("file"))[-1]
            last = (int(match.group("line")), line[match.end():]) if name == basename else None
        elif last is not None:
            # Note lines under a diagnostic (candidate lists) belong to its message.
            last = (last[0], last[1] + "\n" + line)
    return events


def check_fixture_output(rel, legs, log_text, report, suffix=""):
    """Tie every PASS line to the leg that produced it; a leg with no PASS never fired.

    A PASS whose diagnostic line is known goes to the innermost leg whose span holds that line,
    preferring legs whose substring the printed message carries. A PASS whose line lies in no
    span goes to an unfired leg whose substring its message carries. PASS lines with no known
    diagnostic line (parse errors, errors in imports) fill the remaining unfired legs in
    source order.
    """
    for leg in legs:
        leg["hits"] = 0
    basename = rel.rsplit("/", 1)[-1]
    untied = 0
    surplus = 0
    stray = []
    for diag in pass_events(log_text, basename):
        owner = None
        if diag is not None:
            where, message = diag
            holding = [leg for leg in legs if leg["span"][0] <= where <= leg["span"][1]]
            # The leg whose substring the printed message carries; nesting breaks ties
            # (latest-starting span is the innermost).
            named = [leg for leg in holding if leg["sub"] in message] or holding
            if named:
                owner = max(named, key=lambda leg: (leg["span"][0], -(leg["span"][1] - leg["span"][0])))
        if owner is not None:
            owner["hits"] += 1
        elif diag is None:
            untied += 1
        else:
            stray.append(diag[1])
    # A known line outside every span credits only a leg its message names, never any leg.
    for message in stray:
        named = [leg for leg in legs if leg["hits"] == 0 and leg["sub"] in message]
        if named:
            named[0]["hits"] = 1
        else:
            surplus += 1
    for leg in legs:
        if untied and leg["hits"] == 0:
            leg["hits"] = 1
            untied -= 1
    untied += surplus
    for leg in legs:
        if leg["hits"] == 0:
            report.fail("arm-count", rel, leg["line"],
                        "expect_error leg '%s' never fired%s (no PASS line ties to it; a bare leg "
                        "earlier in compile order may have ended the check)" % (leg["sub"][:60], suffix))
        elif leg["hits"] > 1:
            report.note("arm-count", rel, leg["line"],
                        "leg matched %d times%s (duplicate emission)" % (leg["hits"], suffix))
    if untied:
        report.note("arm-count", rel, legs[0]["line"] if legs else 1,
                    "%d PASS line(s) beyond the legs%s (duplicate emission)" % (untied, suffix))


def split_batch_log(text):
    """Per-fixture output of a multi-file --check log, keyed by stem. The compiler ends each
    file's output with 'PASS: <path>' or 'FAIL: <path>'; test_err.bat's single-fixture runs
    (err_cpp_header_parse_budget) end with 'PASS: <stem>'. Text after the last such line is
    keyed ''."""
    segments = {}
    current = []
    for raw in text.splitlines():
        line = raw.strip()
        match = re.match(r"(?:PASS|FAIL): (.+\.cb)$", line) or re.match(r"(?:PASS|FAIL): (err_\w+)$", line)
        if match:
            stem = re.split(r"[\\/]", match.group(1))[-1]
            stem = stem[:-3] if stem.endswith(".cb") else stem
            segments[stem] = segments.get(stem, "") + "\n".join(current) + "\n"
            current = []
        else:
            current.append(raw)
    if current:
        segments[""] = "\n".join(current) + "\n"
    return segments


def read_groups(results_dir, report):
    """test.bat batches: stem -> (rsp index, log path). test_err.bat names the rsp after the
    0-based group index (err_group_<N-1>_of_<M>.rsp) and test.bat names the log after the
    1-based one (test_err_<N>.log). Rsp files from a run with a different group count are a
    failure: they would pair with the wrong logs."""
    group_of = {}
    found = []
    for rsp in glob.glob(os.path.join(results_dir, "err_group_*_of_*.rsp")):
        match = re.match(r"err_group_(\d+)_of_(\d+)\.rsp$", os.path.basename(rsp))
        if match:
            found.append((int(match.group(1)), int(match.group(2)), rsp))
    counts = sorted({m for _, m, _ in found})
    if len(counts) > 1:
        report.fail("arm-count", "results", 1, "rsp files from more than one group count (_of_%s); "
                    "stale batches would mispair with the logs" % "/".join(str(c) for c in counts))
        return group_of, True
    for index, total, rsp in found:
        log = os.path.join(results_dir, "test_err_%d.log" % (index + 1))
        with open(rsp, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                name = re.split(r"[\\/]", line.strip().strip('"'))[-1]
                if name.endswith(".cb"):
                    group_of[name[:-3]] = (index, log)
    return group_of, bool(found)


def outputs_checks(test_dir, results_dir, report, host, skips):
    err_dir = os.path.join(test_dir, "errors")
    fixtures = sorted(glob.glob(os.path.join(err_dir, "err_*.cb")))
    fixtures += sorted(glob.glob(os.path.join(err_dir, "policy", "err_*.cb")))
    group_of, batched = read_groups(results_dir, report)
    if batched and "err_cpp_header_parse_budget" not in group_of:
        # test_err.bat runs it after group 1's batch, in the same log, with --nologo.
        group_of["err_cpp_header_parse_budget"] = (0, os.path.join(results_dir, "test_err_1.log"))
    warm_pass = bool(glob.glob(os.path.join(results_dir, "*.warm.log")))
    split_cache = {}
    for path in fixtures:
        stem = os.path.basename(path)[:-3]
        rel = rel_of(path)
        if stem in skips:
            continue
        flags, expect_exit = load_flags(path + ".flags")
        if expect_exit == "nonzero":
            report.note("arm-count", rel, 1, "contract is the exit code and output substring; legs not checked")
            continue
        target = target_for_flags(flags, host)
        if target is None:
            report.fail("arm-count", rel, 1, "unrecognized -p target in the sidecar; legs cannot be counted")
            continue
        text = read_text(path)
        legs, info = taken_legs(text, target)
        if legs is None:
            line = next(leg["line"] for leg in info["legs"] if leg["eff"] is None)
            report.note("arm-count", rel, line, "if const arm not evaluable on this host; legs not checked")
            continue
        logs = []
        for candidate in (stem + ".log", "policy_" + stem + ".log"):
            full = os.path.join(results_dir, candidate)
            if os.path.isfile(full):
                logs.append(("", read_text(full)))
                break
        if not logs and stem in group_of:
            index, log = group_of[stem]
            if not os.path.isfile(log):
                report.fail("arm-count", rel, 1, "group log %s is missing" % os.path.basename(log))
                continue
            if log not in split_cache:
                split_cache[log] = split_batch_log(read_text(log))
            segments = split_cache[log]
            key = stem
            if stem == "err_cpp_header_parse_budget" and stem not in segments:
                key = ""
            if key not in segments:
                report.fail("arm-count", rel, 1, "no per-file verdict line in %s; the batch stopped "
                            "before this fixture" % os.path.basename(log))
                continue
            logs.append((" (error group %d)" % (index + 1), segments[key]))
        if not logs:
            report.fail("arm-count", rel, 1, "no per-fixture log, group batch or skip entry in the results")
            continue
        if log_has_skip(logs[0][1]):
            report.note("arm-count", rel, 1, "fixture skipped on this host (--isolated unsupported)")
            continue
        if warm_pass and not logs[0][0]:
            warm = os.path.join(results_dir, stem + ".warm.log")
            if os.path.isfile(warm):
                logs.append((" (warm cache)", read_text(warm)))
            elif not os.path.isfile(os.path.join(results_dir, "policy_" + stem + ".log")):
                report.fail("arm-count", rel, 1, "warm-cache pass ran but left no %s.warm.log" % stem)
        for suffix, log_text in logs:
            check_fixture_output(rel, legs, log_text, report, suffix)


# ---------------------------------------------------------------- main

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--test-dir", default=os.path.join(ROOT, "Test"))
    parser.add_argument("--cflat-dir", default=os.path.join(ROOT, "cflat"))
    parser.add_argument("--phase", choices=("static", "outputs", "all"), default="all")
    parser.add_argument("--results", default="", help="suite results dir (writes lint_*.result on failure)")
    parser.add_argument("--platform", choices=("windows", "macos", "linux"), default="")
    parser.add_argument("--skip", default="", help="err fixture stems this host's suite skips (space-separated)")
    options = parser.parse_args()

    host = options.platform or host_target()
    test_dir = os.path.abspath(options.test_dir)
    if not os.path.isdir(test_dir):
        print("LINT: no test dir: %s" % test_dir)
        return 2
    report = Report(os.path.abspath(options.results) if options.results else "")
    if report.results_dir:
        os.makedirs(report.results_dir, exist_ok=True)

    if options.phase in ("static", "all"):
        static_checks(test_dir, os.path.abspath(options.cflat_dir), report, platform_macros(host))
        check_enum_width(test_dir, report)
    if options.phase in ("outputs", "all"):
        if not report.results_dir:
            print("LINT NOTE: outputs phase needs --results; check 1 not run")
        else:
            outputs_checks(test_dir, report.results_dir, report, host, set(options.skip.split()))

    summary = ", ".join("%s %d" % (k, v) for k, v in sorted(report.counts.items())) or "none"
    print("LINT: %d failure(s) [%s] on %s, phase %s" % (report.failures, summary, host, options.phase))
    return 1 if report.failures else 0


if __name__ == "__main__":
    sys.exit(main())
