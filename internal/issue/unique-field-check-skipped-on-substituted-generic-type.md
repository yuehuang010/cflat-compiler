# The `unique` field-shape check does not run on a generically-substituted field type

Severity: silently accepts a field shape the language otherwise forbids. Found 2026-07-20.
Re-investigated 2026-07-20 (see "Why the obvious fix is wrong" - the original fix direction
would break working code).

## Summary

Declaring a `unique` interface ARRAY as a struct field is correctly rejected:

```cflat
class Box { unique IS slots[4] = default; };
// 'unique' on field 'Box.slots' requires a single-indirection pointer type such as 'unique Node* n'
```

A `unique` POINTER array field is rejected too, with a different message:

```cflat
struct Box { unique C* slots[4] = default; };
// 'unique' on field 'Box.slots': fixed arrays are not supported yet - the synthesized
// destructor deletes a single pointer and would leak the rest
```

The identical field shape reached through GENERIC SUBSTITUTION is accepted with no diagnostic:

```cflat
struct Node<V> { V values[4] = default; };   // instantiated with V = unique IS / unique C*
```

`btree_node<K,V>` is exactly this shape.

## Why the obvious fix is wrong

The original fix direction here was "re-validate field shapes at monomorphization time". Do NOT
do that. The rule's stated reason is the SYNTHESIZED destructor: it deletes exactly one pointer
per field and cannot express an N-element release. A generic container that reaches the shape
through substitution does not use the synthesized destructor for those slots - it hand-writes
teardown. `btree<K, unique C*>` (shipping and covered in `Test/test_collection_leaks.cb`) and
`btree<K, unique IFace>` (enabled 2026-07-20) both depend on this: `btree.cb` releases every
value through `_freeValue(move V value)`, and `btree.cb:201` records, as measured design, that
the synthesized destructor does NOT free a unique-substituted array slot. Enforcing the
declaration-time rule per instantiation would reject both containers outright.

So the escape is load-bearing today. What is missing is not the check but the DISTINCTION: the
rule should key on "this field's release is synthesized" rather than on "this field was written
by hand".

## What is actually still open

A generic struct with a `V values[N]` field, `V` unique, and NO hand-written teardown leaks
silently. Nothing diagnoses it and nothing frees it. Two directions, neither attempted:

1. Make the synthesized struct destructor release a `unique` fixed-array field element by
   element - the same walk `LLVMBackend::EmitOwningUniqueArrayCleanup` now does for a `unique`
   array LOCAL. That would let the field rule be relaxed rather than routed around. It is a
   BREAKING change for `btree`: `btree_node`'s synthesized destructor would start freeing
   `values[]`, double-freeing everything `_freeValue` already released. Any attempt must
   migrate `btree.cb` in the same change.
2. Re-run the field rule at monomorphization ONLY for instantiations whose destructor is
   synthesized. Requires knowing, at check time, whether the instantiation hand-writes teardown.

If (1) is taken, confirm the diagnostic names the INSTANTIATION and points at a source location
the user can act on, not at the generic template.

## Related

- The leak this check was meant to prevent (`unique IFace[N]` local freeing nothing) was FIXED
  2026-07-20 in `EmitDestructorsForScope` / `EmitOwningUniqueArrayCleanup`; that issue file is
  deleted. It was never interface-specific - `unique C*[4]` leaked identically.
