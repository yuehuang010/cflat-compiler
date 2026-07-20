# `delete` of a borrow bound to a named local is not rejected

Filed 2026-07-19. Supersedes `alias-list-take-double-free.md`, whose direct-call-result half
is now FIXED.

## Summary

Deleting a borrowed pointer double-frees: the real owner frees it too. The compiler rejects
the DIRECT call-result form, but binding the borrow to a named local first defeats the check
entirely.

```cflat
// CAUGHT
list<alias int*> l; ...;  delete l.get(0);     // 'get' is declared `alias T get(int)`
list<alias int*> l; ...;  delete l.take(0);    // fixed 2026-07-19, see below

// NOT CAUGHT - the gap
list<alias int*> l; int* p = new int; l.add(p);
int* o = l.take(0);        // o == p (same address, verified)
delete o;                  // frees once here, then p's scope-exit auto-free -> SIGABRT

// NOT CAUGHT - and this one is a GUARANTEED double free
list<unique B*> l; l.add(new B());
B* g = l.get(0);
delete g;                  // accepted; the list frees it again at its dtor
```

The second case is the more serious one: it needs no `alias` at all, so this is a GENERAL
hole, not an alias-container problem.

Note the per-variable nulling does NOT save these. `delete o` nulls `o`, but `p` is a
separate variable holding the same address and is not nulled - so its auto-free still runs.
Nulling defends against double-free through ONE name, not against two names for one
allocation.

## What was fixed (2026-07-19)

`take()` on an `alias` container used to return a value that was neither owning NOR marked
as a borrow - a third state the delete check had nothing to key on. `get()` was caught only
because it is declared `alias T get(int)`.

Fix: 3 lines at `cflat/MainListener.h:~2776`. When a written `move` degrades to a no-op on
an `alias` instantiation, it now also sets `declType.IsAlias`, so the result is a marked
borrow. The existing check at `:~12915` then fires unchanged. Zero false positives.

The diagnostic was also made non-circular: it used to advise "use an owning accessor such as
take()/removeAt()" even when `take()` was the callee. A borrowing container has no owning
accessor, so that form now steers to the owner instead.

## Why the named-local half was NOT landed

Spiked 2026-07-19 (patch: `scratch/delete-alias-taint-spike.patch`). The infrastructure
already exists - `NamedVariable::IsBorrowed` is propagated at `~:7814` and consulted by a
`delete` check at `~:12958`; the only gap was that an `alias`-returning call never set
`srcIsBorrowed`. Six lines closed it, and all four repro cases above were then caught.

**It was reverted because of a genuine false positive**, measured at exactly one site in 429
tests: `Test/test_collection_leaks.cb:944`, which is a CORRECT program.
`dictionary<int,int*>` is a borrowed VIEW - `copy()` shallow-copies the pointers, nothing
auto-frees, and the test deliberately deletes the shared pointee exactly once.

## Root cause of the false positive - the real blocker

`IsAlias` records *"came from an accessor"*. The delete-safety question is *"will anyone
else free this?"* Those diverge:

| Container | `get()` is `alias T` | Does the container free it? |
|---|---|---|
| `list<unique B*>` | yes | YES - deleting the borrow double-frees |
| `dictionary<int,int*>` (view) | yes | NO - deleting it is correct |

The accessor's return type carries no record of whether the container owns its elements.
Discriminating on `rightNV.TypeAndValue.IsUnique` was tried and does not work - the flag is
not carried on the return type, so the narrowed rule fired never.

So the rule is broad-or-nothing, and broad rejects correct code.

Also measured: **reassignment does not clear the taint** (`o = new int; delete o;` was
rejected). A conservative rule is already too sticky before flow-sensitivity even enters.

## Fix direction

Propagate element ownership into the monomorphized accessor's return `TypeAndValue`, so a
`delete` site can ask "does the owning container free this?" rather than "did this come from
an accessor?". Once that distinction exists on the return type, the six-line taint change
becomes correct and can be revived from the patch.

Do NOT reuse `TypeAndValue.IsAliasTypeArg` for any of this - it is deliberately kept out of
these checks (see the comment at `cflat/LLVMBackend.h:~456`); reusing it corrupts the
return-borrow machinery.

## Related

- `internal/plan/unique-ownership.md` - the `alias` design.
