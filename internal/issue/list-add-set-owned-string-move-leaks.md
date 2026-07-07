# list.add / list.set of a bare owned `string` (call result or named local) leaks

## Summary

Moving a *bare* owned `string` value into a `list<string>` via `add(move T)` or
`set(int, move T)` leaks the string's heap buffer. "Bare" means the argument is a
call result (`list.add(readIt())`), a named local (`string s = ...; list.add(s)`), or
another owned expression - anything that is NOT a string literal and NOT wrapped in a
`"" + ...` concat.

The buffer ends up owned by NOBODY: the source is marked moved-from (so it is not freed
at scope exit), but the list slot does not take ownership either (clearing the list or
destructing it does not free it). So it leaks.

Only `--heap-audit` flags it; `--asan` (UAF/double-free) stays clean, so it manifests
otherwise as slow growth.

## Repro

```cflat
import "list.cb";
import "string.cb";
string readIt() { return "" + "callresult00"; }   // owned, 13 bytes
extern int main()
{
    list<string> a = default;
    string s = readIt();
    a.add(s);            // LEAK: bare named-local moved into add
    a.add(readIt());     // LEAK: bare call-result moved into add
    a.add("literal");    // OK: string literal
    a.set(0, readIt());  // LEAK: bare call-result moved into set

    a.add("" + readIt());   // OK: concat temp
    a.set(0, "" + s);       // OK: concat temp
    { list<string> x = move a; }   // clear/destruct does NOT free the leaked ones
    return 0;
}
```

`add`/`set` of `list<int>` (non-owning element) and `add`/`set` of a **string literal**
or a **`"" + expr` concat** are all clean. The leak is specific to a bare owned-string
value moved into the `move T` parameter.

## Workaround (in use)

Wrap every owned-string move-arg to `list<string>.add` / `.set` in `"" + ...`:

```cflat
this.tabBuffers.set(i, "" + nativeControlText(EDITOR_KEY));   // not: set(i, nativeControlText(...))
this.tabPaths.add("" + path);                                 // not: add(path)
```

`example/ui/fedit/fedit.cb` (`_saveActiveToModel`, `_addTab`) uses this form; the
comment points here. The gallery's `ComboBox.addItem` / `ListView.addColumn`
(`this.items.add("" + s)`) were already written this way, which is why they never leaked.

## Root cause (hypothesis, unverified)

The move-into-`move T`-parameter path for a value type with a destructor (`string`)
appears to null/skip the source's cleanup (correctly, it was moved) but the parameter -
or the subsequent `_data[index] = move value` store inside list.add/set - does not adopt
the buffer into the slot's ownership. A `"" + x` concat produces a freshly-materialized
temporary that the store DOES adopt, which is why the wrapper works. Likely the same
family as the borrow-arg temp fixes from 2026-06-18, but for the `move T` parameter case.

## Fix direction

In the move-argument lowering for a `move T` value parameter (and/or the `_data[i] =
move value` store), ensure the destination adopts ownership of the moved-in buffer so the
list slot frees it on clear/destruct. Add a regression test mirroring the repro (add +
set of a call result and a named local, then assert leak-clean under --heap-audit).
