# A borrow taken from a temporary escapes silently through a STRUCT FIELD assignment

Filed 2026-08-21 from an external report (a quant backtester built on v0.11.0, macOS arm64,
~3k lines). Reproduced on `39d4b38`, Release.

Severity: **silent use-after-free**. Compiles clean with no diagnostic, links, runs, prints
garbage. The reporter hit it in a CSV loader (`b.date = fields.get(0);`), not in synthesized code.

## Repro

```cflat
import "list.cb";
import "string.cb";
struct Bar { string date = default; };
Bar parseLine(string line) {
    list<string> fields = line.split(",");
    Bar b;
    b.date = fields.get(0);     // borrow escapes into b; fields freed at return
    return b;
}
extern int main() {
    list<Bar> bars;
    for (int i = 0; i < 200; i++) { string ln = "2020-01-0" + i.toString() + ",1,2"; bars.add(parseLine(ln)); }
    for (int i = 0; i < 200; i++) { string junk = "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX" + i.toString(); }
    printf("first=[%s] last=[%s]\n", bars.get(0).date.data(), bars.get(199).date.data());
    return 0;
}
```

Measured (Release, `39d4b38`): compiles with NO diagnostic, and prints

```
first=[] last=[<binary garbage>]
```

The reporter's ASan run (`--asan -g`) reports `heap-use-after-free` inside `strlen`, called from
the first `printf`.

## Why this is a gap and not a policy choice

The alias/escape checker ALREADY catches the same borrow when it is bound to a LOCAL - that is
[[string-field-read-from-container-element-requires-copy]] (the reporter's issue 05):

```cflat
string d = bars.get(0).date;   // ERROR: "cannot bind 'Bar.date' taken from a temporary to a
                               // local; its buffer is owned elsewhere ... use '.copy()'"
```

So the same value, through the same accessor, is rejected on the LOCAL-BINDING path and accepted
on the FIELD-ASSIGNMENT path. One of the two paths is wrong, and the accepted one is the unsafe
one. The store target being a field (which then outlives the container via `return b`) is strictly
MORE dangerous than the local case the checker already rejects.

## Fix direction

The local-binding guard fires from the bind site; the assignment path to a struct field does not
consult it. Route field-assignment stores of a borrowed `string`/buffer-owning value through the
same check that produces the "taken from a temporary" diagnostic, then either
- reject with the same message and point at `.copy()`, or
- implicitly copy (see [[string-field-read-from-container-element-requires-copy]], which argues the
  ergonomic case for the copy on the read side; whichever way that is decided, the two paths must
  agree).

Note the escape here is *provable locally*: `fields` is a local container that dies at the end of
`parseLine`, and `b` is returned. This does NOT need the interprocedural provenance analysis that
[[string-pointer-param-slot-semantics-depend-on-argument-provenance]] and
[[temp-unique-field-escapes-through-an-indirect-callee-or-an-unfollowable-return]] are blocked on -
the pattern-match on "borrow from a temporary/accessor stored into a field" is the same one the
local-binding guard already performs.

## Adjacent

- [[string-field-read-from-container-element-requires-copy]] - the caught half of the same pattern.
- [[temp-unique-field-escapes-through-an-indirect-callee-or-an-unfollowable-return]] - other
  escape shapes the guard family cannot see; unlike those, this one is in-function and decidable.
