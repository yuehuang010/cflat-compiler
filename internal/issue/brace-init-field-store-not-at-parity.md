# Brace-init field stores are not at parity with `=` field stores

Found 2026-07-16. Pre-existing. **All eight instances fixed (seven 2026-07-16, #8 2026-07-17).**
Tracked as a class rather than one-off bugs because it bit eight times. This file is retained as the
durable-fix record: new field-store rules MUST still be shared helpers from the start (see below).

## The root cause

`EmitOneFieldInit` (`cflat/MainListener.h`) is a SECOND, independent field-store path, used by:
- brace-init - `Holder h = { p = t };`
- the `<Tag attr=...>` element sugar

`ParseAssignmentExpression`'s `=` branch is the first. It carries ~400 lines of ownership, alias,
alignment and closure rules. `EmitOneFieldInit` has only ever carried a subset. **Every rule added to
the `=` path silently fails to apply to brace-init unless someone remembers this file exists.**
Nobody has, eight times.

## THE DURABLE FIX (the point of this file)

**New field-store rules MUST be written as shared helpers called from both sites, from the start.**
Seven rules are now extracted this way and cannot drift again:

`RejectFieldAllocAlignMismatch`, `TransferPointerOwnershipOnStore`, `RejectOwningValueCopyIntoField`,
`CloneClosureFromNamedSource`, `RejectAliasStoreIntoField` (+ the Trap A and field-to-field `unique`
rules).

Adding rule number eight inline in the `=` path re-opens this class. Do not.

## Track record

| # | Rule missing from brace-init | Symptom | Status |
|---|---|---|---|
| 1 | Trap A (borrow into a `unique` field) | double-free | fixed 2026-07-16 |
| 2 | field-to-field `unique` copy | double-free | fixed 2026-07-16 |
| 3 | `alignas(0,N)` agreement | **heap corruption** | fixed 2026-07-16 |
| 4 | pointer ownership transfer | double-free | fixed 2026-07-16 |
| 5 | owning-value-by-value reject | ASAN double-free | fixed 2026-07-16 |
| 6 | closure auto-clone of a named source | ASAN use-after-free | fixed 2026-07-16 |
| 7 | alias-into-field reject | accepted a dangling store | fixed 2026-07-16 |
| 8 | `move` string into a field is not honored | leak | fixed 2026-07-17 |

## Instance #8 (fixed 2026-07-17)

`move` on a string into a field via brace-init deep-copied instead of transferring, orphaning the
original buffer:

```cflat
{ string a = "hel" + "lo";
  H h = { s = move a };      // was leaks=1; now leaks=0
}
```

Root cause was subtler than "the move-transfer rule is missing": `ParseMoveExpression` on a string
value type ALREADY zeroes the source storage and returns the captured value with `Storage == nullptr`
and `IsOwningString == true` but **`IsMove` NOT set**. So `EmitOneFieldInit`'s own string deep-copy
branch (gated only on "field is string + value is a struct") still fired and copied the value we
already solely owned, orphaning the captured original buffer.

Fix (two parts, both in `EmitOneFieldInit`):
- The string deep-copy branch now skips when the source already hands us sole ownership:
  `rightNV.TypeAndValue.IsMove || (rightNV.Storage == nullptr && rightNV.IsOwningString)`. A plain
  owned string LOCAL (`Storage != nullptr`, not a move) still deep-copies - the known-benign
  divergence, since the local keeps its buffer.
- The `=` path's move-string `_ptr`-null transfer was extracted VERBATIM into shared helper
  `TransferMoveStringOwnershipOnStore` and is now called from both sites (covers the `move`-param
  shape where `IsMove` is set and `Storage` points at the param slot).

Regression: `Test/test_collection_leaks.cb` ("brace-init move-string" block), HeapAudit oracle at 0.

## Known BENIGN divergence - not a bug, do not "fix"

**Owned-string local into a string field**: `=` **rejects** it; brace-init **accepts and
deep-copies**. Verified ASAN-clean, 0 leaks, source intact. `EmitOneFieldInit` has its own string
deep-copy that makes the store safe, so the `=` path's rejection is unnecessary here. A genuine
behavioural divergence, but a safe one - the brace-init behaviour is arguably the nicer of the two.

**Reassign-free is genuinely NOT needed** in `EmitOneFieldInit`: both callers construct a FRESH slot,
so there is no old value to free. Adding one would free garbage.

## Ordering constraint (discovered the hard way)

The owning-value reject (#5) fires on `__closure_fat_ptr` - it is a struct with a full destructor, so
it satisfies every precondition of the owning-value rule. The `=` path is only saved by its early
`return` after the closure clone. `EmitOneFieldInit` mirrors that with if/else: closure takes the
clone, everything else takes the rejects. **Without this, fixes #5 and #6 silently cancel each
other.** Any new rule must be placed with this in mind.

## Fix direction

Extract each remaining rule from the `=` path into a helper called from both sites. **Extract
VERBATIM** so `=` semantics stay byte-identical - the 2026-07-16 pass verified this by building the
baseline compiler from a stash and comparing emitted IR across 10 test files (all byte-identical),
including the one `=` case whose codegen flows through an extracted helper.

**Do NOT attempt a wholesale port** of the `=` path into `EmitOneFieldInit`. Considered and rejected:
~400 lines of interacting rules, several correctly inapplicable to a fresh slot. Incremental
extraction, one rule at a time, each with its own regression test.

## Related

- `internal/plan/unique-ownership.md` (Part I) - instances 1-4 and the shared-helper precedent.
