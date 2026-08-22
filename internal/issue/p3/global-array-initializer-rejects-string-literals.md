# A global `string[N]` cannot be initialised from string literals

Filed 2026-08-21 from an external report (v0.11.0 issue 13 - a static table of ticker symbols).
Reproduced on `39d4b38`.

## Repro

```cflat
string[2] g_symbols = { "AAPL", "MSFT" };
```
```
global array initializer elements must be compile-time constants
```
```cflat
string[] g_symbols = { "AAPL", "MSFT" };
```
```
array-view initializer '= {}' is not allowed at global scope
```

## Narrowing (measured on 39d4b38)

Global array initializers are NOT broken in general - these both compile and run:

```cflat
int[3] g_ints = { 1, 2, 3 };
const char* g_names[2] = { "AAPL", "MSFT" };
```

So the gap is specific to `string`, i.e. an element type that owns a heap buffer. The message is
also misleading: the elements here ARE compile-time constants (string literals live in the string
pool); what is not constant is the `string` STRUCT that has to be built around each one, which
needs a runtime initializer.

## Fix direction

1. **Preferred:** emit a static initializer for globals whose element type needs construction -
   the same mechanism a global `string g = "AAPL";` already uses, applied per element. Check
   whether that single-value case works today; if it does, the array case is the same code in a
   loop and this is small.
2. Failing that, fix the diagnostic to say what is actually wrong and name the workaround: "a
   global array of 'string' cannot be initialised at file scope; use `const char*[]` for a static
   table, or populate a `list<string>` at startup". The current text sends the reader looking for
   a non-constant element that is not there.

The `const char*[2]` form above is the workaround, and is genuinely fine for a fixed symbol table -
worth mentioning in the docs alongside whichever fix lands.
