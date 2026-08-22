# Papercuts from the MemPressMonitor port: raw strings, `-o` output directory

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 13). Four
items were reported; two are actionable, two are rulings recorded here so they are not re-filed.

## 1. No raw-string opt-out from interpolation - actionable

String interpolation applies to EVERY literal, so emitting JSON meant doubling every structural
brace (`{{` / `}}`) in strings that contain no interpolation at all. Not wrong, but a format
template or a JSON blob is exactly the content where the escaping is densest and the intent is
"none of this is code".

Fix direction: a raw-string prefix that suppresses interpolation for that literal, e.g.
`r"{ \"a\": 1 }"`. Lexer-level, no effect on any existing literal. Related but distinct:
[[json-ish-brace-literal-still-typed-string]].

## 2. `-o out/x.exe` fails when `out/` does not exist - actionable, small

Measured on `cd847a3`:

```
Error: output directory 'scratch/triage/nodir' does not exist (-o scratch/triage/nodir/x.exe).
```

The reporter filed this as a nit and noted every other compiler they use behaves the same way. The
counter-argument, and the reason it is worth doing: `--init`-style tooling sets an expectation that
CFlat is friendlier than that, and creating the leaf directory is a two-line change. The message is
already good - it names the directory and the flag - so if the ruling is to keep failing, this
closes as WONTFIX with no work.

## 3. Adjacent string literals are not concatenated - BY DESIGN, do not re-file

The reporter accepts the design choice (and praises the diagnostic - see
[[diagnostic-attribution-and-reserved-word-wording]]), but notes the cost: long `printf` format
strings cannot be broken across lines. If (1) lands, a raw string plus `+` covers the readability
case. No change proposed to the concatenation rule itself.

## 4. No incremental compilation - MOVED to its own file

Split out and measured as [[no-incremental-build-and-no-up-to-date-check]], because the two cases
have very different fix costs: a no-op rebuild needs only an mtime up-to-date check (cheap, worth
doing), while per-file reuse is plan-level work.
