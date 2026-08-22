# A chained method call whose intermediate result is a PRIMITIVE re-uses the base object as receiver

Filed 2026-08-21 from an external report (quant backtester, v0.11.0). Reproduced and narrowed on
`39d4b38`, Release.

Severity: **silent miscompile when an overload happens to match the base type**; a confusing
overload-resolution error otherwise.

## Repro A - silently calls the WRONG method (no diagnostic)

```cflat
struct Box { int v = 7; int get() { return v; } int dbl() { return v*2; } };
int dbl(int x) { return x * 100; }
extern int main() { Box b; printf("%d\n", b.get().dbl()); return 0; }
```

Expected `700` (`dbl(7)`, the free function on the `int` result). Measured: **`14`** - it called
`Box.dbl()` on `b`. The result of `get()` is discarded and the base object is passed as the
receiver. Compiles clean, exit 0.

## Repro B - the shape the reporter hit (hard error)

```cflat
import "list.cb";
import "string.cb";
extern int main() {
    list<int> xs; xs.add(1); xs.add(2);
    string bad = xs.count().toString();   // fails
    int n = xs.count(); string ok = n.toString();   // works
    return 0;
}
```

```
no overload of 'toString' matches the given arguments.
  Call arguments (1):
    [0] list__i32 <unnamed>
```

The receiver is the `list`, not the `i32` that `count()` returned.

## Narrowing (measured on 39d4b38)

| Chain | Result |
|-------|--------|
| `b.get().dbl()`, `xs.count().toString()`, `s.length().toString()` - method whose result is a PRIMITIVE, then a method | **BROKEN** (base used as receiver) |
| `s.trim().length()` - method whose result is a STRUCT, then a method | OK |
| `o.inner()->get()` - pointer chain | OK |
| `twice(3).toString()` - FREE function returning a primitive, then a method | OK |
| `int n = xs.count(); n.toString()` - hoisted into a local | OK |

So the trigger is narrow and specific: **method-call receiver resolution when the previous link in
the chain is a method call returning a primitive (non-struct, non-pointer) type**. Struct results
and free-function results both resolve correctly, which is why this was not caught earlier - the
common `a.b().c()` chains in the test suite return structs.

## Repro C - the reporter's follow-up (issue 09), same root cause

```cflat
import "list.cb";
import "string.cb";
extern int main() {
    list<int> xs; xs.add(7);
    int v = xs.get(0); string ok = v.toString();   // works
    string bad = xs.get(0).toString();             // fails: receiver is list__i32
    return 0;
}
```

Filed separately by the reporter as "generalizes 04: ANY chained call on a method result that came
from a GENERIC CONTAINER misresolves the receiver". That framing is too narrow in one direction and
too wide in the other - the narrowing table above is the accurate rule. `xs.get(0)` returns `alias
i32`, i.e. a primitive, which is why it fails; a generic container is not required (the `Box` case
in repro A is a plain struct), and a generic container whose accessor returns a STRUCT chains fine.

## Fix direction

In the postfix-chain handling in `MainListener.h`, the receiver for link N+1 is taken from the base
expression rather than from link N's result when link N's result is a primitive - most likely a
path that only re-seats the receiver when the result is an aggregate/pointer (i.e. when there is an
lvalue address to hand on), and falls back to the original base otherwise. A primitive rvalue needs
a temporary slot so it can be the `self` argument, the same way `int n = xs.count(); n.toString()`
materializes one.

Repro A is the important one to keep: it is a wrong-answer bug, not just a rejected program, and it
will silently pick a same-named method on the base type.

## Regression test

Extend an existing test (`Test/test_generics.cb` or `Test/test_operators.cb`) with repro A asserting
`700`, plus `xs.count().toString() == "2"`.

## Second report, 2026-08-21 (MemPressMonitor Win32 port, v0.11.0 issue 01)

Independently hit by a second external project, in the exact repro B shape:
`ids.count().toString()` -> "no overload of 'toString' matches", receiver rendered as the container.
They called it the only outright miscompile-class defect found in a ~2.0k-line port, and named
`.count().toString()` / `.length().toString()` as natural idioms given UFCS plus interpolation.
Same workaround (bind to a local first). Raises the priority; nothing new about the root cause.

The literal-receiver case ([[method-call-on-string-literal-receiver-rejected]]) is likely the same
re-seating defect - check whether one fix covers both.
