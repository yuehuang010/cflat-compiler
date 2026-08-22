# Parse errors attach to the FOLLOWING token, and reserved-word collisions never say "reserved"

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 12).
Reproduced on `cd847a3`, Release.

## Symptom A - a reserved word used as an identifier reports the token but not the reason

```cflat
extern int main() { int package = 3; return package; }
```
```
t_res.cb(1,24): error: mismatched input 'package' expecting {'move', '(', Identifier}
    extern int main() { int package = 3; return package; }
                            ^
hint: missing ';' at end of statement
```

The reader is told `package` is not an `Identifier` without being told WHY - that it is a reserved
word. "`package` is a reserved word in CFlat and cannot be used as an identifier" is immediate.

The set of affected words and the root cause (inline string literals in the import rules of
`CFlat.g4` become implicit lexer tokens that shadow `Identifier` everywhere) are already recorded
in [[import-clause-words-globally-reserved]] - fix that and this symptom disappears for those
words, but the WORDING fix is worth having independently for every genuine keyword.

## Symptom B - the caret lands past the real mistake, and the hint is wrong

Both of these attach the error to the token AFTER the offending construct and then suggest a
missing semicolon that is not missing:

- `p = default;` -> caret on the `=`, `hint: missing ';' at end of statement`
  (see [[default-is-initializer-only-and-is-not-an-expression]])
- `int package = 3;` -> `hint: missing ';' at end of statement` on a line with a `;`

An always-appended "missing `;`" hint is worse than no hint: it is wrong more often than it is
right on these shapes, and it sends the reader to the previous line. The hint should be emitted
only when the recovery actually inserted a semicolon, not as a default suffix on every
`mismatched input`.

## The counter-example worth preserving

From the same reporter, cited as the best diagnostic they hit:

```
adjacent string literals are not concatenated. Join them with the '+' operator, e.g. "a" + "b".
```

It names the rule AND the fix. That is the target shape for the two above. Prior diagnostics work
is recorded in the completed q04 bucket ("Diagnostics wording and attribution", `9062709`) - this
is the next round of the same, driven by a second external report.

## Regression test

`Test/errors/` is the home: an `err_reserved_word_identifier.cb` asserting the new wording via
`expect_error("reserved word")`.
