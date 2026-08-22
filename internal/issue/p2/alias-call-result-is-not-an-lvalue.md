# An `alias T` call result is not treated as an lvalue: no address-of, no field assignment

Filed 2026-08-21 from an external report (v0.11.0 issues 08 and 14). Reproduced on `39d4b38`.
Two symptoms, one root cause: `list<T>::get` is declared `alias T get(int index)`
(`cflat/core/list.cb:125`) and hands back a BORROW of storage the container owns - but the call
result carries no `Storage`, so every lvalue operation on it is refused while plain READS work.

## Symptom A - address-of (issue 08)

A chart `Series { list<double>* y }` view over an element owned by a longer-lived list:

```cflat
import "list.cb";
struct Curve  { list<double> eq = default;  list<double>* eqPtr() { return &eq; } };
struct Series { list<double>* y = nullptr; };
extern int main() {
    list<Curve> curves; Curve c; c.eq.add(1.0); curves.add(c);
    Series s1; s1.y = curves.get(0).eqPtr();   // ACCEPTED
    Series s2; s2.y = &curves.get(0).eq;       // REJECTED
    return 0;
}
```

```
(11,22): Unable to get an Address-of an object without a Storage.
```

The two lines produce the same pointer, into the same element storage, with the same lifetime. One
compiles; the other does not. A user who wants the pointer just writes the accessor - so the rule
is not enforcing anything, it is charging boilerplate.

## Symptom B - assigning a field of an element (issue 14)

```cflat
import "list.cb";
struct Row { int a = 0; double eps = 0.0; };
extern int main() {
    list<Row> rows; Row r; r.a = 1; rows.add(r);
    printf("%f", rows.get(0).eps);   // READ - accepted
    rows.get(0).eps = 2.02;          // rejected
    rows[0].eps = 2.02;              // rejected too (same via operator[])
    return 0;
}
```

```
Left side of assignment is not an addressable lvalue.
```

Updating one field of a struct element in place is ordinary code. The reporter's workaround is
copy the struct, mutate the copy, `list.set(i, move copy)` - a full element copy per field write.
Note the asymmetry within the same expression: `rows.get(0).eps` READS fine and cannot be assigned.

## Narrowing (measured on 39d4b38)

The rule is **address-of any CALL RESULT**, not anything specific to fields or to `alias`:

| Expression | Result |
|------------|--------|
| `&local.eq` (field of a local struct) | OK |
| `&curves.get(0)` (the element itself) | rejected, same message |
| `&curves.get(0).eq` (field of the element) | rejected, same message |
| `curves.get(0).eqPtr()` (accessor method) | OK |
| `rows.get(0).eps` as a READ | OK |
| `rows.get(0).eps = v` / `rows[0].eps = v` as an ASSIGNMENT | rejected, "not an addressable lvalue" |

`get` returns a BORROW of storage the container owns, not a temporary. Neither the address-of
check nor the assignment lvalue check sees that: the call result carries no `Storage`, so both
treat it as an rvalue. Reads work because they never ask for an address.

## Is the accepted path unsafe? Not observed.

Probed deliberately: took `s.y` from element 0, then grew `curves` by 256 elements (forcing
reallocation) and churned the heap with 500 string allocations. `s.y->count()` and `s.y->get(0)`
still returned the correct `1` / `1.0`. So this is filed as an inconsistency/ergonomics issue, not
a latent use-after-free - unlike [[borrow-from-temp-escapes-into-struct-field]], which does
dangle. Worth re-probing if the list growth strategy ever changes to move elements in place.

## Fix direction

1. **Preferred:** give an `alias T` call result the element's storage, so it behaves as the lvalue
   it already is. That fixes BOTH symptoms at once - `&curves.get(0).eq` becomes legal for the same
   reason the accessor is, and `rows.get(0).eps = v` becomes a plain store into the element - and
   keeps the paths consistent. The borrow rules that already apply to the accessor's return then
   apply here too, unchanged. This is the shape C++ gets from `T& operator[]`, and `alias T` is
   already spelled as exactly that contract.
2. If (1) is judged too broad, at minimum fix the diagnostics. "Unable to get an Address-of an
   object without a Storage" is internal vocabulary and is actively misleading here - the element
   plainly has storage inside the list. Say "cannot take the address of a call result" and point at
   the accessor-method idiom.

Do NOT close this by rejecting the accessor path instead - returning `&field` from a method is a
normal, useful idiom and is how the reporter worked around it.

## Second report, 2026-08-21 (MemPressMonitor Win32 port, v0.11.0 issue 02)

Independently hit by a second external project, which named it the HIGHEST TOTAL COST item of the
whole port - and it produced a silent wrong answer, not just friction. `xs.get(0) = 5` gives "Left
side of assignment is not an addressable lvalue", so the correct idiom for a `list<Struct>`
accumulator is read-copy-write:

```cflat
AppEntry app = apps->get(i).copy();
app.workingSet = app.workingSet + process->workingSet;
apps->set(i, move app);
```

Their FIRST workaround was a hand-rolled remove/insert rotation that was subtly wrong and reported
one app at 2,722 MB against a true 13 MB. The compiler cannot catch that, and the language offers
no safe alternative - which is the argument for fix direction (1) below over a diagnostics-only
fix. Their requests, in preference order, match it: a `ref`-returning `xs.at(i)`, or `operator[]`
as an lvalue, or at minimum an in-place `xs.update(i, fn)`.

Adjacent: [[list-has-no-insert]] (the same reporter reached for insert while building the
workaround).
