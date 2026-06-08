# Regular Expressions (`core/regex.cb`)

CFlat ships a regular-expression engine written entirely in CFlat - no compiler
support, just `import "regex.cb"`. A pattern is parsed once into an AST, compiled
to a Thompson NFA, and matched by a **Pike VM** that simulates all active threads
one input byte at a time. Matching is **linear-time O(n*m)** in the input length
`n` and pattern size `m`, with **no catastrophic backtracking** - the safe model
that RE2 and Go ship. A pattern such as `(a+)+b` that hangs a naive backtracking
engine on adversarial input is decided here in microseconds.

The engine supports submatch **capture groups**, **case-insensitive** matching,
and **replace** / **split** helpers.

## Table of Contents

- [Quick start](#quick-start)
- [Escaping pattern literals](#escaping-pattern-literals-important)
- [Supported syntax](#supported-syntax)
- [Matching semantics](#matching-semantics)
- [API reference](#api-reference)
- [Examples](#examples)
- [Limitations](#limitations)
- [How it works](#how-it-works)

**Related:** [Language & Core Library Features](LANGUAGE.md)

---

## Quick start

```c
import "regex.cb";

extern int main()
{
    Regex r;
    r.compile("(\\d+)-(\\d+)");          // see escaping note below

    Match m = r.find("call 123-4567 now");
    if (m.success)
    {
        printf("%s\n", m.value("call 123-4567 now").data());   // 123-4567
        printf("%s\n", m.group(1, "call 123-4567 now").data()); // 123
        printf("%s\n", m.group(2, "call 123-4567 now").data()); // 4567
    }

    // one-off, no Regex object:
    bool hit = Regex.isMatch("\\d+", "abc123");                 // true
    return 0;
}
```

A `Regex` is **compiled once** and can be matched against many inputs - reuse it
in a loop rather than recompiling per call.

---

## Escaping pattern literals (IMPORTANT)

CFlat runs **every string literal** through format-string interpolation and
escape decoding *before* your program ever sees the bytes. Two regex characters
collide with that processing, so when you write a pattern as a **string literal**
you must escape them:

| You want the engine to see | Write in a CFlat literal | Why |
|----------------------------|--------------------------|-----|
| `\d` `\w` `\s` `\b` (and `\.` `\(` ...) | `\\d` `\\w` `\\s` `\\b` (`\\.` `\\(`) | `\` starts a CFlat string escape; double it. |
| `a{2,3}` (bounded repeat) | `a{{2,3}}` | An unescaped `{` starts `{expr}` interpolation; `{{`/`}}` folds to a literal `{`/`}`. |
| a literal backslash `\` | `\\\\` | Four backslashes -> two in source's eyes -> one byte. |

This is the same situation as regex-in-string in C, Java, or Python without raw
strings. Patterns that are **not** string literals - built at runtime, read from a
file, or passed in by a user - are *not* folded and use the raw spelling:

```c
string pat = readPatternFromConfig();   // "\d{3}" with single backslash, raw braces
Regex r;
r.compile(pat);                          // no doubling needed
```

The engine itself fully understands `\d` and `{n,m}`; only the *source spelling*
of a literal needs the doubling.

---

## Supported syntax

| Construct | Meaning |
|-----------|---------|
| `abc` | literal characters |
| `.` | any byte except newline (`\n`) |
| `*` `+` `?` | zero-or-more, one-or-more, zero-or-one (greedy) |
| `{n}` `{n,}` `{n,m}` | exactly n, n-or-more, between n and m |
| `\|` (i.e. `a\|b`) | alternation |
| `( ... )` | capturing group (also groups for precedence) |
| `^` `$` | start-of-input / end-of-input anchors |
| `[abc]` `[a-z]` | character class / range |
| `[^...]` | negated character class |
| `\d \w \s` | digit `[0-9]`, word `[0-9A-Za-z_]`, whitespace |
| `\D \W \S` | the negations of the above |
| `\b` | word boundary (zero-width) |
| `\n \t \r \f` | newline, tab, carriage return, form feed |
| `\.` `\*` `\(` ... | a literal metacharacter |

Inside a class, `-` is literal when first (`[-a]`) or last (`[a-]`), the first
`]` is literal (`[]...]`), and `\d \w \s` shorthands are allowed (`[\d.]`).

Capture groups are numbered **1..9** by the order of their opening `(`; group 0 is
the whole match. Groups beyond 9 still affect precedence but are not captured.

---

## Matching semantics

The engine uses **leftmost-first greedy** semantics, the same model as
Perl/PCRE/Python/JavaScript (and Go's default `regexp`):

- **Leftmost**: the match with the earliest start position wins.
- **First (priority)**: at a given start, the highest-priority thread wins.
  Alternation prefers the **earlier** branch and quantifiers are **greedy**
  (prefer to consume more).

The practical consequence is that alternation order matters:

```c
Regex.find("a|ab|abc", "xabc");   // matches "a"  (start 1, end 2)
```

The earlier alternative `a` wins even though `abc` would match more. This is
**not** POSIX leftmost-longest - if you need the longest alternative, order your
alternation longest-first (`abc|ab|a`).

Greedy quantifiers still take as much as they can:

```c
Regex.find("\\d+", "abc123");     // matches "123" (greedy, end of digit run)
```

`isMatch` is unanchored - it succeeds if the pattern matches **anywhere** in the
input. Use `^...$` to require a full-string match.

---

## API reference

### `Regex`

| Method | Description |
|--------|-------------|
| `bool compile(string pattern)` | Compile a pattern. Returns false (and sets `ok()` false) on a malformed pattern. |
| `bool compileIgnoreCase(string pattern)` | Compile case-insensitively (ASCII letters). |
| `bool ok()` | False if the last compile failed. All match queries then return no match. |
| `int groupCount()` | Number of capturing groups in the pattern. |
| `bool isMatch(string input)` | True if the pattern matches anywhere in `input`. |
| `Match find(string input)` | The leftmost match (`.success` is false if none). |
| `list<Match> findAll(string input)` | All non-overlapping matches, left to right. |
| `string replace(string input, string repl)` | Replace every match with `repl` (`$0..$9`, `$$`). |
| `list<string> split(string input)` | Split `input` around matches of the pattern. |

Static one-offs (compile a throwaway `Regex` internally):

| Method | Description |
|--------|-------------|
| `static bool Regex.isMatch(string pattern, string input)` | Compile + test in one call. |
| `static Match Regex.find(string pattern, string input)` | Compile + find in one call. |

### `Match`

| Member | Description |
|--------|-------------|
| `bool success` | Whether a match was found. |
| `int start` / `int end` | Byte half-range `[start, end)` of the whole match (== group 0). |
| `int groupStart(int g)` / `int groupEnd(int g)` | Group `g`'s offsets, or -1 if it did not participate. |
| `bool groupMatched(int g)` | Whether group `g` participated in the match. |
| `string group(int g, string input)` | Group `g`'s substring (group 0 = whole match); empty if absent. |
| `string value(string input)` | The whole matched substring (alias for `group(0, input)`). |

`group`, `value`, `replace`, `split`, and `_substr` return **owning** strings
(freshly allocated); the caller owns the buffer.

> **Note on `Match` and `input`**: a `Match` stores byte offsets, not the text.
> Pass the *same* input string to `group`/`value` that you matched against.

---

## Examples

### Capture groups

```c
Regex r;
r.compile("(\\w+)@(\\w+)");
Match m = r.find("user@host");
m.group(1, "user@host");   // "user"
m.group(2, "user@host");   // "host"
```

A group inside a quantifier captures its **last** iteration:

```c
Regex r;
r.compile("(ab)+");
Match m = r.find("ababab");
m.value("ababab");      // "ababab"
m.group(1, "ababab");   // "ab"  (last iteration)
```

An optional group that did not participate reports `groupMatched(g) == false`:

```c
Regex r;
r.compile("(a)(b)?c");
Match m = r.find("ac");
m.groupMatched(2);      // false
```

### Iterating all matches

```c
Regex r;
r.compile("\\d+");
list<Match> ms = r.findAll("a12b345c");
int i = 0;
while (i < ms.count())
{
    printf("%s\n", ms.get(i).value("a12b345c").data());   // 12, then 345
    i++;
}
```

### Case-insensitive

```c
Regex r;
r.compileIgnoreCase("hello");
r.isMatch("HeLLo");          // true
r.isMatch("say HELLO!");     // true (unanchored)
```

### Replace (with group references)

```c
Regex r;
r.compile("(\\w+)@(\\w+)");
r.replace("user@host", "$2.$1");    // "host.user"

Regex digits;
digits.compile("\\d+");
digits.replace("a1b22c333", "#");   // "a#b#c#"
```

`$0` is the whole match, `$1`..`$9` are groups, and `$$` is a literal `$`.

### Split

```c
Regex comma;
comma.compile(",");
list<string> parts = comma.split("a,b,c");   // ["a", "b", "c"]

Regex ws;
ws.compile("\\s+");
list<string> words = ws.split("the  quick   fox");   // ["the", "quick", "fox"]
```

### Full-string validation

```c
Regex date;
date.compile("^\\d{{4}}-\\d{{2}}-\\d{{2}}$");
date.isMatch("2026-06-07");   // true
date.isMatch("2026-6-7");     // false
```

---

## Limitations

The following are **not** supported in this version:

- Backreferences (`\1`) and lookahead / lookbehind (`(?=...)`, `(?<=...)`).
- Non-greedy / lazy quantifiers (`*?`, `+?`).
- Non-capturing `(?:...)` and named groups `(?<name>...)`.
- More than 9 capture groups (extras affect precedence but are not captured).
- Unicode classes. CFlat strings are byte buffers, so classes and `.` operate on
  bytes; ASCII works as expected, multi-byte UTF-8 is matched byte-by-byte.

Malformed patterns (unbalanced `(`, unterminated `[`, `{3,1}`) do not crash - they
leave the `Regex` with `ok() == false`, and all match queries return no match.

---

## How it works

```
pattern string
   -> recursive-descent parser  -> AST (list<ReNode> pool, integer-indexed)
   -> Thompson NFA construction -> states (list<NfaState> pool)
   -> Pike VM simulation        -> Match (+ capture vectors)
```

- **Character classes** are 256-bit `ByteSet`s (four `u64` words), so a class test
  is a couple of shifts and an AND - no per-character branching.
- **NFA states** are one of: *consume* (match a `ByteSet`, advance), *split* (two
  epsilon edges, the first higher priority), *assert* (`^` `$` `\b`, zero-width),
  *save* (record a capture offset), or *accept*.
- **The Pike VM** keeps the set of active threads, each with its own capture
  vector, and advances them in lockstep over the input. A per-step
  generation-stamp array deduplicates states so the active set never exceeds the
  NFA size - this is what bounds the work to O(n*m) and eliminates backtracking.
- **Leftmost-first** falls out of processing threads in priority order and cutting
  lower-priority threads once a higher-priority thread accepts.

See `cflat/core/regex.cb` for the implementation and `Test/test_regex.cb` for the
behavioral test suite (148 cases).
