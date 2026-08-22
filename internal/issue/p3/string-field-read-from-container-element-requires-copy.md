# Reading a `string` field off a container element requires `.copy()` - ergonomics of the (correct)
# alias diagnostic

Filed 2026-08-21 from an external report (v0.11.0 issue 05). Reproduced on `39d4b38`. The current
behaviour is CORRECT - this is an ergonomics item, not a bug.

## Repro

```cflat
import "list.cb";
import "string.cb";
struct Bar { string date = default; double close = 0.0; };
extern int main() {
    list<Bar> bars;
    Bar b; b.date = "2020-01-01"; b.close = 1.0; bars.add(b);
    string d = bars.get(0).date;                 // rejected
    string msg = "date=" + bars.get(0).date;     // rejected
    return 0;
}
```

```
cannot bind 'Bar.date' taken from a temporary to a local; its buffer is owned elsewhere and would
be freed out from under the local. Bind the whole call result first (e.g. `auto t = ...;`) or use
'.copy()' for an independent copy.
```

The diagnostic is accurate and actionable. The complaint is frequency: "read a string field out of
an element of a container" is one of the most common operations in ordinary data-processing code,
and every occurrence needs `.copy()` or an intermediate binding. The reporter (a ~3k-line
backtester) hit it repeatedly.

## The important part: the sibling path is NOT checked

The same borrow assigned to a struct FIELD used to compile clean and use-after-free -
[[borrow-from-temp-escapes-into-struct-field]] (p1). The p1 fix now rejects that sibling path,
including named brace initialization; this file remains the ergonomics follow-up for the
explicit `.copy()` required by the read-side diagnostics.

## Fix direction (options, not a ruling)

1. **Implicit copy for a `string` bound out of a temporary.** Makes both repro lines just work, and
   would also make the p1 case safe by construction. Cost: a silent allocation where the user may
   have wanted a view; contrary to the "borrow by default, compiler infers copy/move" philosophy
   only in that the copy here is not free.
2. **Allow the read when the result is immediately CONSUMED rather than bound** - the
   `"date=" + bars.get(0).date` concatenation never outlives the full expression, so the escape
   the guard is protecting against cannot happen. This is the narrower and probably better fix:
   it kills the common half of the annoyance with no new allocation and no policy change.
3. Leave as-is and document the `.copy()` idiom prominently in the container docs.

Option 2 is recommended as the first step; it is decidable from the expression shape alone.

## Adjacent

- [[borrow-from-temp-escapes-into-struct-field]] - the unchecked sibling. p1.
