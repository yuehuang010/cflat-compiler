#!/usr/bin/env python3
"""Static inventory of localizable compiler diagnostics.

Scans the C++ sources for LogErrorMessage() call sites and writes the complete
set of English templates, their argument expressions, and their source
locations directly into cflat/locales/en-pseudo.json. This is the
completeness half of the localization pipeline: the runtime
'--locale pseudo --update-locale' pass only sees diagnostics that an error test
actually provokes, so it cannot enumerate the catalog on its own.

Run order matters. The compiler pass is the only source of real observed
argument values, and it rewrites the catalog without the extra fields this
script adds, so run it FIRST and this script LAST:

    cflat --locale pseudo --update-locale en-pseudo --locale-dir cflat/locales \
          --check Test/errors/err_*.cb -i Test/library
    python3 utilities/extract_diagnostics.py --report

The script carries every existing argumentExamples entry forward, so nothing
the compiler observed is lost; it only adds the keys no test reaches.

normalize_key() below is a port of NormalizeKey in
cflat/DiagnosticLocalization.cpp. The C++ version remains the source of truth:
every run re-derives the key of every entry already in the catalog and reports
any disagreement, so a change on either side is caught immediately.

Usage:
    python3 utilities/extract_diagnostics.py                 # -> cflat/locales/en-pseudo.json
    python3 utilities/extract_diagnostics.py --out foo.json
    python3 utilities/extract_diagnostics.py --report        # summary to stderr
    python3 utilities/extract_diagnostics.py --strict        # exit 1 on any problem
"""

import argparse
import glob
import json
import os
import re
import sys

CALL_NAME = "LogErrorMessage"
LEGACY_NAME = "LogError"

# (state machine) Walk C++ text once, yielding only positions that are real
# code - not inside a comment, string, or character literal.
def scan_code_positions(text):
    positions = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            i = text.find('\n', i)
            if i < 0:
                break
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            end = text.find('*/', i + 2)
            i = n if end < 0 else end + 2
            continue
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        positions.append(i)
        i += 1
    return positions


def find_call_starts(text, code_positions, name):
    code_set = set(code_positions)
    starts = []
    idx = text.find(name)
    while idx >= 0:
        end = idx + len(name)
        before_ok = idx == 0 or not (text[idx - 1].isalnum() or text[idx - 1] in "_")
        after_ok = end < len(text) and not (text[end].isalnum() or text[end] in "_")
        if before_ok and after_ok and idx in code_set:
            starts.append(idx)
        idx = text.find(name, idx + 1)
    return starts


def next_code_char(text, pos, code_set):
    i = pos
    while i < len(text):
        if i in code_set and not text[i].isspace():
            return i
        i += 1
    return -1


def prev_code_token(text, pos):
    i = pos - 1
    while i >= 0 and text[i].isspace():
        i -= 1
    end = i + 1
    while i >= 0 and (text[i].isalnum() or text[i] in "_:"):
        i -= 1
    return text[i + 1:end]


# Returns the text between the call's parentheses, or None if unbalanced.
def extract_call_arguments(text, open_paren):
    depth = 0
    i = open_paren
    n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            i = text.find('\n', i)
            if i < 0:
                return None
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            end = text.find('*/', i + 2)
            if end < 0:
                return None
            i = end + 2
            continue
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n:
                if text[i] == '\\':
                    i += 2
                    continue
                if text[i] == quote:
                    i += 1
                    break
                i += 1
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:i]
        i += 1
    return None


def split_top_level(argument_text):
    parts = []
    depth = 0
    current = []
    i = 0
    n = len(argument_text)
    while i < n:
        c = argument_text[i]
        if c == '"' or c == "'":
            quote = c
            start = i
            i += 1
            while i < n:
                if argument_text[i] == '\\':
                    i += 2
                    continue
                if argument_text[i] == quote:
                    i += 1
                    break
                i += 1
            current.append(argument_text[start:i])
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ',' and depth == 0:
            parts.append(''.join(current).strip())
            current = []
            i += 1
            continue
        current.append(c)
        i += 1
    tail = ''.join(current).strip()
    if tail or parts:
        parts.append(tail)
    return parts


ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0',
           '"': '"', "'": "'", '\\': '\\'}


# Concatenates adjacent C++ string literals ("a" "b") into one value.
# Returns None when the expression is not a pure literal sequence.
def parse_string_literal_sequence(expression):
    out = []
    i = 0
    n = len(expression)
    saw_literal = False
    while i < n:
        c = expression[i]
        if c.isspace():
            i += 1
            continue
        if c != '"':
            return None
        i += 1
        while i < n:
            ch = expression[i]
            if ch == '\\':
                i += 1
                if i >= n:
                    return None
                out.append(ESCAPES.get(expression[i], expression[i]))
                i += 1
                continue
            if ch == '"':
                i += 1
                break
            out.append(ch)
            i += 1
        else:
            return None
        saw_literal = True
    return ''.join(out) if saw_literal else None


def count_placeholders(template):
    count = 0
    i = 0
    while i < len(template) - 1:
        if template[i] == '{':
            if template[i + 1] == '{':
                i += 2
                continue
            if template[i + 1] == '}':
                count += 1
                i += 2
                continue
        i += 1
    return count


def line_of(text, offset):
    return text.count('\n', 0, offset) + 1


def looks_like_declaration(prev_token, argument_text):
    if prev_token.endswith("::") or prev_token in ("void", "virtual", "inline"):
        return True
    return "std::string englishTemplate" in argument_text


def scan_file(path, problems):
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        text = handle.read()

    code_positions = scan_code_positions(text)
    code_set = set(code_positions)
    name = os.path.basename(path)
    entries = []

    for start in find_call_starts(text, code_positions, CALL_NAME):
        paren = next_code_char(text, start + len(CALL_NAME), code_set)
        if paren < 0 or text[paren] != '(':
            continue
        argument_text = extract_call_arguments(text, paren)
        if argument_text is None:
            problems.append("%s:%d: unbalanced argument list" % (name, line_of(text, start)))
            continue
        if looks_like_declaration(prev_code_token(text, start), argument_text):
            continue

        site = "%s:%d" % (name, line_of(text, start))
        parts = split_top_level(argument_text)
        if not parts:
            problems.append("%s: empty argument list" % site)
            continue

        template = parse_string_literal_sequence(parts[0])
        if template is None:
            problems.append("%s: template is not a string literal: %s"
                            % (site, ' '.join(parts[0].split())[:80]))
            continue

        arg_names = []
        known_values = {}
        if len(parts) > 1:
            arg_expression = parts[1].strip()
            if arg_expression.startswith('{') and arg_expression.endswith('}'):
                for index, item in enumerate(split_top_level(arg_expression[1:-1])):
                    if not item:
                        continue
                    literal = parse_string_literal_sequence(item)
                    if literal is not None:
                        # A literal argument is its own example value.
                        arg_names.append(literal)
                        known_values[str(index)] = literal
                    else:
                        arg_names.append(' '.join(item.split()))
            elif arg_expression:
                arg_names = None
                problems.append("%s: argument list is not a brace-init list: %s"
                                % (site, ' '.join(arg_expression.split())[:80]))

        placeholders = count_placeholders(template)
        if arg_names is not None and placeholders != len(arg_names):
            problems.append("%s: %d placeholders but %d arguments"
                            % (site, placeholders, len(arg_names)))

        entries.append({
            "template": template,
            "argNames": arg_names if arg_names is not None else [],
            "knownValues": known_values,
            "site": site,
            "placeholders": placeholders,
        })

    legacy = []
    for start in find_call_starts(text, code_positions, LEGACY_NAME):
        paren = next_code_char(text, start + len(LEGACY_NAME), code_set)
        if paren < 0 or text[paren] != '(':
            continue
        argument_text = extract_call_arguments(text, paren)
        if argument_text is None:
            continue
        if looks_like_declaration(prev_code_token(text, start), argument_text):
            continue
        if "std::string message" in argument_text:
            continue
        legacy.append("%s:%d" % (name, line_of(text, start)))

    return entries, legacy


def merge_entries(all_entries):
    merged = {}
    for entry in all_entries:
        key = entry["template"]
        existing = merged.get(key)
        if existing is None:
            merged[key] = {
                "template": key,
                "argNames": entry["argNames"],
                "knownValues": entry["knownValues"],
                "placeholders": entry["placeholders"],
                "sites": [entry["site"]],
            }
            continue
        existing["sites"].append(entry["site"])
        if not existing["argNames"] and entry["argNames"]:
            existing["argNames"] = entry["argNames"]
        for index, value in entry["knownValues"].items():
            existing["knownValues"].setdefault(index, value)
    for record in merged.values():
        record["sites"].sort()
    return [merged[key] for key in sorted(merged)]


# Port of NormalizeKeyFull in cflat/DiagnosticLocalization.cpp. Verified on
# every key of the existing catalog on each run; a mismatch is reported as a
# problem rather than silently written.
def normalize_key_full(template):
    key = []
    argument = 0
    i = 0
    while i < len(template):
        if template[i] == '{' and i + 1 < len(template) and template[i + 1] == '}':
            key.append("arg%d" % argument)
            argument += 1
            i += 2
            continue
        c = template[i]
        i += 1
        if c.isascii() and c.isalnum():
            key.append(c.lower())
    return ''.join(key)


def is_compacted_key(key):
    if len(key) != 59 or key[20:23] != "...":
        return False
    return all(c in "0123456789abcdef" for c in key[43:])


# Port of CompactKey: FNV-1a over the full normalized key.
def compact_key(full_key):
    if len(full_key) <= 40 or is_compacted_key(full_key):
        return full_key
    hash_value = 14695981039346656037
    for byte in full_key.encode('utf-8'):
        hash_value ^= byte
        hash_value = (hash_value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return "%s...%s%016x" % (full_key[:20], full_key[-20:], hash_value)


def normalize_key(template):
    return compact_key(normalize_key_full(template))


def numbered_template(template):
    out = []
    argument = 0
    i = 0
    while i < len(template):
        if template[i:i + 2] == '{}':
            out.append('{%d}' % argument)
            argument += 1
            i += 2
            continue
        out.append(template[i])
        i += 1
    return ''.join(out)


def source_template(numbered):
    return re.sub(r'\{\d+\}', '{}', numbered)


# Builds the en-pseudo catalog: complete key set from the scan, argument
# examples carried over from the existing catalog (the runtime
# '--update-locale' pass is the only source of real observed values).
def build_catalog(records, existing, problems):
    messages = {}
    examples = {}
    arg_names = {}
    sites = {}
    key_owner = {}

    for record in records:
        key = normalize_key(record["template"])
        if key in key_owner and key_owner[key] != record["template"]:
            problems.append("key collision '%s': %r and %r"
                            % (key, key_owner[key], record["template"]))
            continue
        key_owner[key] = record["template"]

        messages[key] = numbered_template(record["template"])
        if record["argNames"]:
            arg_names[key] = record["argNames"]
        sites[key] = record["sites"]

        carried = existing.get("argumentExamples", {}).get(key)
        if carried:
            examples[key] = carried
        elif len(record["knownValues"]) == record["placeholders"] and record["placeholders"]:
            # Every argument is a literal at the call site, so it is its own example.
            examples[key] = [record["knownValues"][str(i)]
                             for i in range(record["placeholders"])]

    # Any catalog key the scan no longer produces is a stale entry: report it
    # rather than carrying dead text forward.
    for key, value in existing.get("messages", {}).items():
        if key not in messages:
            problems.append("stale catalog entry dropped: %s" % value[:70])

    catalog = {
        "locale": "en-pseudo",
        "messages": messages,
        "argumentExamples": examples,
        "argumentNames": arg_names,
        "sites": sites,
    }
    return catalog


# The catalog is written by two producers; make sure the Python key derivation
# still agrees with the C++ one on every entry the compiler wrote.
def verify_keys_against(existing, problems):
    checked = 0
    for key, value in existing.get("messages", {}).items():
        derived = normalize_key(source_template(value))
        if derived != key:
            problems.append("key derivation mismatch: catalog '%s' vs derived '%s' for %r"
                            % (key, derived, value[:60]))
        checked += 1
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("sources", nargs="*",
                        help="source files to scan (default: cflat/*.cpp cflat/*.h)")
    parser.add_argument("--out", default=os.path.join("cflat", "locales", "en-pseudo.json"),
                        help="output catalog path (default: cflat/locales/en-pseudo.json)")
    parser.add_argument("--report", action="store_true",
                        help="print a coverage/problem summary to stderr")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 if any call site could not be extracted cleanly")
    options = parser.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sources = options.sources
    if not sources:
        sources = (sorted(glob.glob(os.path.join(root, "cflat", "*.cpp")))
                   + sorted(glob.glob(os.path.join(root, "cflat", "*.h"))))
    if not sources:
        sys.stderr.write("no sources to scan\n")
        return 1

    problems = []
    all_entries = []
    legacy_sites = []
    for path in sources:
        entries, legacy = scan_file(path, problems)
        all_entries.extend(entries)
        legacy_sites.extend(legacy)

    records = merge_entries(all_entries)

    out_path = options.out if os.path.isabs(options.out) else os.path.join(root, options.out)
    existing = {}
    if os.path.exists(out_path):
        with open(out_path, 'r', encoding='utf-8') as handle:
            existing = json.load(handle)

    verified = verify_keys_against(existing, problems)
    catalog = build_catalog(records, existing, problems)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'w', encoding='utf-8', newline='\n') as handle:
        json.dump(catalog, handle, indent=2, sort_keys=True, ensure_ascii=False)
        handle.write('\n')

    if options.report or options.strict:
        with_examples = len(catalog["argumentExamples"])
        total = len(catalog["messages"])
        sys.stderr.write("scanned %d files\n" % len(sources))
        sys.stderr.write("templates: %d unique, %d call sites\n" % (total, len(all_entries)))
        sys.stderr.write("argument examples: %d of %d (%d carried from the previous catalog)\n"
                         % (with_examples, total, len(existing.get("argumentExamples", {}))))
        sys.stderr.write("key derivation verified against %d existing entries\n" % verified)
        sys.stderr.write("unmigrated LogError call sites: %d\n" % len(legacy_sites))
        if options.report:
            for site in sorted(legacy_sites):
                sys.stderr.write("  LogError  %s\n" % site)
            for key in sorted(catalog["messages"]):
                if not catalog["argumentExamples"].get(key):
                    sys.stderr.write("  NO EXAMPLE  %s  %s\n"
                                     % (catalog["sites"][key][0], catalog["messages"][key][:70]))
        for problem in problems:
            sys.stderr.write("  PROBLEM  %s\n" % problem)

    if options.strict and problems:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
