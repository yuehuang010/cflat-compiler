# A `.copy()` result consumed mid-chain is never freed

Filed 2026-08-09 while fixing [[lambda-body-owning-temp-never-destructed]]; measured on master
`0669ebc` and unchanged by that fix. Independent of lambdas - it reproduces in a plain free
function - and it is the residue that kept the lambda cell from reaching zero.

Severity: **leak**, 32 bytes per evaluation. No dangle, no wrong value.

## Repro

```cflat
struct SBox { string s = default; };
int n = 3;
SBox makeStr() { SBox b = default; b.s = "hello" + "world" + n.toString(); return b; }
int ff() { return makeStr().s.copy().length(); }
extern int main() { printf("len=%d\n", ff()); return 0; }
```

`leaks --atExit`: **32 total leaked bytes**, compile rc 0, run rc 0, prints `len=11`.

Measured in all three spellings and on both binaries (warm `--init-local` on each side):

| spelling                                            | pre | post |
|-----------------------------------------------------|-----|------|
| free function `ff()` above                            | 32 B | 32 B |
| block-body lambda `() => { return ...copy()...; }`    | 32 B | 32 B |
| expression-body lambda `() => makeStr().s.copy()...`  | 64 B | 32 B |

The expression-body row's extra 32 bytes were the `SBox` temp itself and are what
[[lambda-body-owning-temp-never-destructed]] fixed; the surviving 32 bytes are this issue, and
the three spellings now agree exactly.

## What it tells you

`.copy()` returns a fresh owned `string` whose only consumer is the immediately following
`.length()` call. That result is a call-produced owning temp with no named owner, so it should
be registered on the owned-STRING temp list and freed by the end-of-full-expression
`FlushOwnedTemps`. It is not: the value is consumed as a method RECEIVER, and the receiver path
(`RegisterOwningTempReceiver`) excludes `typeName == "string"` outright, while
`RegisterDiscardedOwningStructTemp` only sees a whole discarded statement result.

## Fix direction

Give the string receiver the same treatment the struct receiver already gets: when a method's
receiver is a call-produced owning `string` with no storage, spill it and register it as an
owned STRING temp (`RegisterOwnedStringTemp`) rather than skipping it. Check the
`MethodConsumesReceiver` exclusion applies for strings too, and check the neighbouring owning
value types (`list<T>`, `dictionary<K,V>`) for the same hole before scoping - the receiver-path
type exclusions are the thing to enumerate, not this one call.
