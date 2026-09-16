# Using a reserved keyword as a variable name produces an unrelated parse error

`doc/LANGUAGE.md` already concedes this ("If you accidentally use a reserved word as an
identifier, the error message may point to a nearby token rather than the reserved word
itself"). In practice the cascade is long enough that the cause is not obvious.

## Repro

```cflat
import "cruntime.cb";
extern int main()
{
    char[64] where = default;
    snprintf(&where[0], 64, "%d", 1);
    return 0;
}
```

```
(4,17): error: found 'where' but expected {'move', '(', Identifier}
(5,18): error: cannot understand the code at '&where'
```

Nothing names `where` as a reserved word. Hit for real in
`scratch/dogfood/lang/interp.cb` (a `char[64] where` buffer for a "at line N col M"
suffix); `where` is not a word a C programmer expects to be taken.

## Fix direction

At the point the parser reports `found '<tok>' but expected {... Identifier}`, check
whether `<tok>` is one of the hard keywords. If it is, emit instead: `'where' is a reserved
keyword and cannot be used as a name here` (plus the existing expected-set as context).
Same treatment for the follow-on "cannot understand the code at ..." so only one error is
reported. The list of hard keywords is already fixed in the ANTLR lexer, so the lookup is a
static table.
