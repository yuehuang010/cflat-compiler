# Interface conformance appears to match the FIRST declared overload, not the best match

Filed 2026-07-20. Found while adding `set(int, move T)` to `cflat/core/list.cb`.

## Summary

When a class declares two overloads of a contract method - one matching the interface and one
differing only in `move`-ness - conformance passes or fails depending on DECLARATION ORDER.
The plain overload must be declared first. If the `move` overload comes first, the check
compares it against the interface's plain signature and rejects the class, even though a
conforming overload exists later in the same body.

## Repro

`IList<T>` declares `void set(int index, T value)` (no `move`). In `cflat/core/list.cb`:

```cflat
// REJECTED - move overload declared before the plain one
if const (!is_pointer(T)) { void set(int index, move T value) { ... } }
void set(int index, T value) { ... }
```

```
string.cb(182,20): class 'list__string' method 'set': parameter 'value' is declared 'move'
but interface 'IList__string' does not declare it 'move' - a call through the interface does
not transfer ownership, so the caller still frees the argument that class 'list__string' has
already taken over (a double free). Drop 'move' from class 'list__string', or declare the
parameter 'move string value' on interface 'IList__string'.
```

Reordering fixes it, with no other change:

```cflat
// ACCEPTED - plain first, move overload after
void set(int index, T value) { ... }
if const (!is_pointer(T)) { void set(int index, move T value) { ... } }
```

`add` has an identically shaped pair (`add(T)` plus a guarded `add(move T)`) and is accepted -
because the plain one happens to be declared first.

## Why it is a trap

The diagnostic is confident and specific, and its advice is wrong for this case: it tells you
to drop `move` from the class or add `move` to the interface, when the actual fix is to move a
declaration. Nothing in the message hints at ordering. A maintainer following the advice would
either delete a needed overload or wrongly put `move` into a contract that must not have one.

`cflat/core/list.cb` currently carries a comment on the `set` block explaining the required
order. That comment is the only thing preventing a future reorganisation from reintroducing the
failure.

## Root cause direction

Find the conformance walk (the check emitting "is declared 'move' but interface ... does not
declare it") and see how it selects the class-side candidate for a given interface method. It
looks like a first-match-by-name-and-arity lookup rather than a search for any overload whose
full signature satisfies the contract. The fix is to consider every overload with that name and
accept the class if ANY of them conforms - reporting the mismatch only when none does.

## Related

- `cflat/core/list.cb` - the `set` / `set(move)` pair and its ordering comment.
- `internal/plan/unique-ownership.md` live item 6 - the code review that found this.
