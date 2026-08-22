# `dictionary<K,V>` cannot be enumerated: no `keys()`, no entry iterator

Filed 2026-08-21 from an external report (MemPressMonitor Win32 port, v0.11.0 issue 06).
Confirmed on `cd847a3`: `cflat/core/dictionary.cb` exposes `add`, `set`, `get`, `operator[]`,
`contains`, `remove`, `count`, `clear`, `copy` - and nothing that yields the keys or the entries.

## Cost

"For every mapping recorded so far" is a routine shape (grouping, aggregation, cache sweeps). With
no iteration the caller has to maintain a PARALLEL `list<K>` of keys purely to drive the loop -
duplicated state that has to be kept in sync by hand at every `add`/`remove`, and that silently
drifts when one call site forgets. The reporter did exactly this in their process grouper.

## Fix direction

The backing store is already a parallel `_status`/`_keys`/`_values` array set with linear probing,
so iteration is a scan over occupied slots. Add, in preference order:

1. **`foreach` support** over the dictionary itself, yielding an entry with `key`/`value`, matching
   the `for-in` element-BORROW semantics already ratified for containers (the loop variable is a
   borrow of the slot; assigning through it writes to the element).
2. **`list<K> keys()`** as the minimum viable API - a copy of the keys, so mutation during
   iteration is well-defined by construction.

Constraints:

- Key ownership: for an OWNING key (`string`, or a struct owning buffers) `keys()` must COPY, and
  an entry iterator must hand back a BORROW - never a move out of the slot, which is the hazard the
  `unique`-key rejection at `cflat/core/dictionary.cb:38` already guards.
- Mutation during iteration invalidates on rehash. State the rule in the docs; if a cheap guard is
  available (a modification counter checked by the iterator), prefer failing loudly over UB.

`hashset<T>` has the same gap and should get the same treatment in the same change.

## Regression test

Extend `Test/test_dictionary.cb`: iterate a dictionary with primitive keys and one with `string`
keys, assert the visited set matches what was inserted (order-independent), assert `count()`
agrees, and check the string-key leg leak-clean.
