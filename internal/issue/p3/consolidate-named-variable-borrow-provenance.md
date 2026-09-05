# Consolidate fragmented borrow provenance and source classification

Filed 2026-08-13 during the integrated Q01-Q15 review.

## Summary

The Q fixes correctly preserved more ownership facts, but represented each new fact as another
parallel group of fields on `NamedVariable` and another set of source-classification locals in the
listeners. The representation now makes propagation, retirement, shadowing, reset, and diagnostic
wording separate obligations for every borrow kind.

Examples on `NamedVariable` include:

- `BorrowsOwnedString` plus `OwnedStringBorrowBlock` and `OwnedStringBorrowFunction`.
- `BorrowsOwnedElement`, its container/external-owner fields, block, and function.
- `IsBonded`, `BondByAddress`, `BondedSources`, block, and function.
- Interface-box provenance, borrowed-by-value parameter provenance, field-path provenance, and
  raw-array provenance as independent flags and strings.

`MainListener_Declarations.cpp` and `MainListener_Expressions.cpp` then reconstruct overlapping
facts into large groups of `srcIs...`, `srcBorrows...`, `srcPointsTo...`, and `srcRaw...` locals.
Adding a new assignment door requires manually copying the right subset.

This is a maintainability issue with correctness impact: missing one propagation or retirement arm
silently changes an ownership decision, and the compiler has no type-level way to require that all
members of one provenance fact move together.

## Simplification direction

Introduce a small value object for binding provenance, for example:

```cpp
struct BorrowProvenance
{
    BorrowKind Kind = BorrowKind::None;
    std::string Origin;
    std::vector<std::string> Sources;
    llvm::BasicBlock* EstablishedIn = nullptr;
    llvm::Function* Function = nullptr;
    bool ThroughField = false;
};
```

The exact shape needs design, but the important properties are:

1. One assignment copies or clears a whole provenance fact.
2. One helper decides whether a rebind may retire it.
3. Declaration initialization and reassignment consume the same classified RHS result.
4. Diagnostics render from the provenance kind and origin rather than unrelated flags.
5. Runtime ownership state, such as a raw-array count, stays out of this compile-time record.

Do this incrementally by migrating one already-paired family first (owned string and owned element
borrow facts are the closest twins), keeping behavior unchanged and running the full host suite.


## Scope additions from the round-2 queue (q01 member 9, recorded 2026-09-04)

This was the last member of the round-2 ownership bucket (q01); every other member landed
2026-09-03/04: e2c4a1e (delete of a `move T*` param honours the count), 53a176e (slot null
store before the call), cb3f71b (aligned unique fields/globals desugar), d7af90d (move-return
temp count), 4223c66 (move-param reassign drops alignment -> rejection, recording was unsound
under conditional reassignment), e04729c (aligned local after plain move-in), 458c6a9
(alignment provenance holes), 966e3d3 (alignment doors + pointer ternary join).

Two more ledgers to absorb, both added by later fixes:
- `globalAssignBorrowOrigin_` and `globalAssignBorrowedAddress_` (global-storage borrow
  provenance, from the delete-of-proven-borrow rejection 455c7e3).
- The open launder `T* q = move p;` off an `&stack` borrow (also from 455c7e3) - close it here,
  not with a one-off door.

Record from the bucket, keep: `move T*` is a COUNTED owning pointer at the ABI level
(`ParameterCarriesRawArrayCount`, `.raw_array_count` slot, honoured by scope-exit cleanup and
forwarding). Only explicit `delete` and `unique<T>` adoption ever ignored the slot; both now read
it. Do not re-open the "downgrade `move T*` to bare `T*`" model - a capability removal needing
its own ruling. Runtime ownership state (the count) stays out of the compile-time provenance
record (property 5 above). Alignment provenance (`AllocAlignKnown`/`AllocAlignValue`, set at
the producing site) is a candidate member of the same value object; the global door fires only
on PRECISE source provenance (advisor call 2026-09-04) - preserve that when migrating.

Sequencing: refactor last, after the doors above are stable; behaviour unchanged, full host
suite as the bar.
