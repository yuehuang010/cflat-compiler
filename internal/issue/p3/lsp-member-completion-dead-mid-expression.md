# LSP: member completion after '.' returns nothing when the expression parses cleanly

Found 2026-08-16 while verifying the stale-completion fix (it was "variant 4" in
the sister project's report, but it is NOT a staleness bug - it reproduces in a
fresh session).

## Summary

Completion requested right after the dot works when the member access is
INCOMPLETE at end of line (`s.` then newline - the classic just-typed-a-dot
state), but returns an empty list when the same position sits inside an
expression that parses cleanly (`s.|length();` - cursor after the dot with the
member text already present). Editors hit this whenever the user re-opens
completion inside existing code, e.g. to change `.length()` to something else.

## Repro (fresh session each time; file = the Demo/add sample with `string s`)

Line `    s.length();`, requests at line 11 (0-based), against Release cflat:

| col | context | result |
|---|---|---|
| 5 | after `s`, before dot | 93 items (global prefix match on "s") |
| 6 | right after the dot | empty |
| 7+ | inside/after `length` | empty |

Control: the same file with the line replaced by `    s.` (parse error) returns
the 3 `list__string` members at col 6.

## Findings so far (probed on current master)

- `extractReceiverAt` (`cflat/LspServer.cpp:221`) is NOT the problem: at col 6
  it yields receiver "s", partial "" in both cases; with the cursor inside
  `length` it yields receiver "s", partial "len..." - also fine.
- The index (per `--symbol-dump line:11-13` on the parseable file) records the
  variable as `s : string` - the ALIAS spelling from the declaration.
- Member symbols are keyed under the CANONICAL instantiated name
  (`list__string._capacity` etc.), and `LspSymbolIndex::LookupPrefix` is an
  exact `starts_with` - so `HandleCompletion`'s
  `LookupPrefix("string." + partial)` matches nothing, and its only fallback
  (`StripGenericSuffix`) does not map an alias to its canonical type.
- Since the broken-line control DOES work, some path taken only under the
  incomplete-expression parse (likely error recovery for a trailing dot)
  re-registers `s` with the canonical type `list__string`. Locating that path
  explains the asymmetry; it is why casual testing (type a dot, see members)
  never catches this.

## Fix direction

Resolve the variable's recorded type through the alias table before the member
lookup in `HandleCompletion` (`cflat/LspServer.cpp:1237` area) - e.g. consult
the index's TypeAlias symbols or register variables with their canonical type
everywhere (find why the error-recovery path and the clean path record
different spellings, and make them agree). Cover hover too if it shares the
lookup. Guard with the fresh-session repro above at col 6 AND col 7+ (partial
word after the dot), plus the existing `s.`-at-EOL control.
