# A JSON-ish brace literal is still typed `string`, so it cannot initialize a `char*`

Filed 2026-08-06 by `fix/brace-literal`, which closed the EMPTY-pair face of
`string-literal-containing-braces-retyped-as-string` and deliberately left this one open.

Severity: false rejection with a misleading message; no miscompile (the variadic/`char*`-parameter
face is now diagnosed, see the `fix/brace-literal` landed record in [[interface-issue-queue]]).

## Repro

```cflat
extern int main(){ char* j = "{\"k\":1}"; return j[0]; }
```

```
repro.cb(1,29): cannot initialize pointer 'j' with a value of type 'char' - the right-hand side
must be a pointer (call getPtr() or use '&')
```

`string j = "{\"k\":1}";` works and prints `{"k":1}`, so the content is handled correctly - only
the literal's TYPE is wrong. `printf("{\"k\":1}\n");` is now a hard error naming the `char*`
parameter (it printed binary garbage before `fix/brace-literal`).

## Root cause

`ClassifyBrace` (`cflat/MainListener_PostfixExpression.cpp`) returns `Verbatim` for matched braces
whose content starts with `"` or `\` - it cannot be an expression, so `ParseFormatString` copies the
region verbatim. `HasInterpolation` still reports true for that kind, so the literal takes the
format path and comes out a `string`.

## Why it was not closed with the empty-pair face

Because the two paths disagree about `{{` / `}}` INSIDE such a region, and the corpus depends on it.
`ParseFormatString` folds `}}` at top level too, but copies a `Verbatim` region's interior
differently from the plain path's `ProcessRawText(..., foldBraces=true)`. `Test/test_reflect.cb`'s
`toJson_nested` expectation ends in `...,\"zip\":12345}}` and means two real closing braces - routing
it down the plain path folds them and the test fails (measured: `toJson_nested FAILED`). So moving
`Verbatim` off the format path is not a one-line change.

## Fix direction

Decide the brace-escape story for a literal that is not interpolated, then make one path implement
it. Either (a) stop folding `{{`/`}}` on the plain path when the literal is not interpolated - which
changes documented behaviour (`doc/LANGUAGE.md`, "Embedding literal braces in strings") and needs its
own sweep; or (b) keep folding and let `Verbatim` regions fold too, which forces `}}}}` in JSON
templates and breaks `test_reflect`'s literal as written. Neither is free; do not attempt it as a
by-product of another fix.

## Test coverage

`Test/test_reflect.cb` `toJson_nested` is the accept-set leg that pins today's behaviour - it fails
the moment a `Verbatim` region starts folding braces.
