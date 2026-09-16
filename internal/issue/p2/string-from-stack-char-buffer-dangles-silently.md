# `string s = &buf[0]` on a stack `char[]` borrows and dangles, with no diagnostic

The compiler already rejects storing an ALIAS string (a `list<string>` element) into a
longer-lived slot:

```
cannot store an 'alias' string local into a longer-lived location; its buffer is owned
elsewhere and would be freed out from under the field. Use '.copy()' for an independent copy.
```

The same store from a stack `char[N]` buffer is silently accepted, and the resulting
`string` points at dead stack memory.

## Repro

```cflat
import "cruntime.cb";
import "string.cb";
import "list.cb";
struct Holder { string s = default; };
void fill(Holder* h, int n)
{
    char[16] buf = default;
    snprintf(&buf[0], 16, "v%d", n);
    h->s = &buf[0];          // borrows the stack buffer - accepted, no diagnostic
}
extern int main()
{
    list<Holder> hs = default;
    for (int i = 0; i < 4; i++) { Holder h = default; fill(&h, i); hs.add(h); }
    for (int i = 0; i < 4; i++) printf("[%s]\n", hs[i].s.data());
    return 0;
}
```

Compiles clean (rc=0) and prints four lines of garbage bytes.

`scratch/dogfood/lang/repro_dangle.cb`. Hit for real in
`scratch/dogfood/lang/interp.cb`, where a one-char operator token was formatted into a
`char[2]` local and stored into `Token.text`: the tokens were all garbage at use time and
nothing in the build said so. The failure looked like a lexer bug for several minutes.

## Root cause (hypothesis)

The implicit `const char*` -> `string` wrap produces a BORROWED string (`_ptr` aimed at the
source bytes, no owned buffer). For a literal that is correct - the bytes are static. For a
local `char[N]` the bytes die at scope exit, but the wrap carries no provenance, so the
existing "alias string stored into a longer-lived location" rule never sees it.

## Fix direction

Give the `char*` -> `string` wrap the same alias provenance the check already understands
when the source is a local array (address-of a stack `char[N]`, or a pointer derived from
one), and route it into the existing longer-lived-store diagnostic, naming `.copy()`. The
literal case must stay silent - only a wrap whose source is provably stack storage should
fire. A narrower first cut that still closes the surprising case: fire on a store into a
struct FIELD or a container element, not on a same-scope local.

Confirmed independently 2026-09-16 on master 24b86c32: repro_dangle compiles clean and prints garbage bytes.
