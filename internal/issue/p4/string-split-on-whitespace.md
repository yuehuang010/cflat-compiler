# Convenience: split a string on whitespace

`string.split` has exactly two overloads: `split(string sep)` and `split(i8 sep)`. Neither
handles the most common everyday case - break a line or a file into words - because real
text separates words with runs of mixed ' ', '\t', '\r', '\n', and both overloads also
produce an empty element for every repeated separator.

Hit while writing a word-frequency counter (`scratch/dogfood/lib/wordfreq.cb`) from the
docs: `text.split(' ')` silently produced tokens like `"away\nthe"`, which then counted as
distinct words from `"away"` and `"the"`. Nothing errors; the output is just quietly wrong.
The workaround is `text.toLower().replace("\n", " ").split(' ')` plus a `trim()` and an
emptiness test per token - and it still breaks on a tab.

`trim()` already defines ASCII whitespace as `' ' | '\t' | '\r' | '\n'`, so the predicate
exists in `string.cb`; only the split does not use it.

## Proposed spelling

```cflat
move list<string> splitWhitespace(string self);   // runs collapse, no empty elements
```

## Alternatives

- `splitAny(string chars)` - general, covers CSV-with-mixed-delimiters too, but the caller
  has to spell out `" \t\r\n"` for the common case.
- `split(string sep, bool removeEmpty)` - smallest addition, but does not address the
  mixed-delimiter half of the problem.
- Do nothing and document the `replace`-then-`split` workaround in `doc/LANGUAGE.md`.

## Acceptance

Needs a maintainer ruling on the surface (which of the three, and the exact name) before
anyone builds it. Once ruled: implement in `cflat/core/string.cb`, extend an existing
`Test/test_string*.cb` with a mixed-whitespace and repeated-separator case.
