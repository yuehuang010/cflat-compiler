# Import-clause words (`from`, `lib`, `cache`, `define`, ...) are globally reserved identifiers

Filed 2026-08-21 from an external report (v0.11.0 issue 03: `int f(int from, int to)`). Reproduced
and widened on `39d4b38`.

## Repro

```cflat
int sliceLen(int from, int to) { return to - from; }
```
```
error: no viable alternative at input 'int sliceLen(int from'
```

It is not just `from`. Every word spelled as an inline literal in the import rules of `CFlat.g4`
becomes an implicit ANTLR lexer token and therefore shadows `Identifier` everywhere in the
language. Measured - each of these fails as a plain local variable name
(`extern int main() { int W = 1; return W - 1; }`):

`from`, `package`, `program`, `cache`, `lib`, `define`, `framework`, `pri`

```
error: mismatched input 'from' expecting {'move', '(', Identifier}
```

Several of these (`from`, `lib`, `cache`, `define`) are extremely ordinary identifier names, and
none of them are in the reserved-keyword list in `doc/LANGUAGE.md`.

## Root cause

`CFlat.g4:713-718` and `CFlat.g4:791` use inline string literals:

```antlr
importDeclaration
    : Import importGroup (As Identifier)? libClause? frameworkClause? defineClause* cacheClause? ';'
    | Import 'program' StringLiteral As Identifier ';'
    | Import 'package' StringLiteral ...
fromClause
    : 'from' StringLiteral
    ;
```

ANTLR promotes each literal to its own token type with higher priority than `Identifier`, so the
lexer can never produce `Identifier("from")` in any context.

## Fix direction

Make them SOFT keywords, the same way `move` is handled: match `Identifier` in the grammar rule
and text-compare in the listener, rather than introducing a lexer token. E.g.

```antlr
fromClause : Identifier StringLiteral ;   // listener asserts the text is "from"
```

with the equivalent change for `program` / `package` / `lib` / `framework` / `define` / `cache` /
`pri`. These only ever appear in a fixed position inside an `import` line, so a text check in the
import handler is unambiguous. Per CLAUDE.md, a new soft keyword must NOT be added to the ANTLR
lexer - the existing import literals are the violation of that rule, retroactively.

Cheaper partial alternative if the grammar change is judged risky: keep the tokens but add them to
the identifier rule (`Identifier | 'from' | 'lib' | ...`) so they are usable as names, and document
the remaining ambiguity. Either way, `doc/LANGUAGE.md`'s reserved list should be corrected - today
it under-reports.

## Regression test

`Test/errors/` is the wrong home (this should COMPILE). Add a function using `from`, `lib`,
`cache`, `define` as parameter names to an existing positive test.
