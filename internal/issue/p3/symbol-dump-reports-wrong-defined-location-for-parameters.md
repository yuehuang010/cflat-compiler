# `--symbol-dump` reports the wrong `defined:` location for a function parameter

A parameter resolves to some unrelated same-named symbol - in the observed case a local
inside a core library in a completely different file - instead of to its own declaration.
Locals are reported correctly, so this is specific to parameters.

## Repro

`scratch/dogfood/lib/repro_10.cb`:

```cflat
int f(int head)
{
    return head;
}
extern int main() { return f(1); }
```

```
x64/Release/cflat scratch/dogfood/lib/repro_10.cb --symbol-dump function:f
```

## Observed

```
line 3: int f(int head)
  f  (function)
    defined: .../scratch/dogfood/lib/repro_10.cb:3
  head (variable) : int
    defined: .../x64/Release/core/page_pool.cb:81
```

`page_pool.cb:81` is `_PP_Link* head = (_PP_Link*)page;` - an unrelated local in a core
library that happens to share the name. In the same dump, body locals (`total`, `cur` in
`scratch/dogfood/lib/tooling.cb`) get their correct line.

## Expected

`head (variable) : int` should report the parameter's own declaration line (line 3 here).
The type is right; only the location is wrong.

## Impact

`--symbol-dump` shares the symbol index with the LSP, so go-to-definition on a parameter
plausibly jumps into an unrelated core file. Worth checking whether the LSP path shows the
same behaviour before sizing the fix.

## Fix direction

Parameters are apparently not registered into `LspSymbolIndex` at their declaration site,
so the by-name lookup falls through to whatever else in the index carries that name (with
no same-file preference to break the tie). Two candidate fixes, probably both: register
parameter declarations in the index when a function body is entered, and make the lookup
prefer a definition in the file being dumped before any cross-file match.
